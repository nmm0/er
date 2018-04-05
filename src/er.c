#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "mpi.h"

#include "kvtree.h"
#include "kvtree_util.h"
#include "redset.h"
#include "shuffile.h"

#include "er.h"
#include "er_util.h"

#define ER_STATE_NULL    (0)
#define ER_STATE_CORRUPT (1)
#define ER_STATE_ENCODED (2)

typedef struct {
  MPI_Comm comm_world;
  MPI_Comm comm_store;
} erset;

static int er_scheme_counter = 0;
static int er_set_counter = 0;

static kvtree* er_schemes = NULL;
static kvtree* er_sets = NULL;

/* define the path to the er file */
static void build_er_path(char* file, size_t len, const char* path)
{
  snprintf(file, len, "%s.er", path);
}

/* define the path to the shuffile file */
static void build_shuffile_path(char* file, size_t len, const char* path)
{
  snprintf(file, len, "%s.shuffile", path);
}

/* define the path to the redset file for the specified rank */
static void build_redset_path(char* file, size_t len, const char* path, int rank)
{
  snprintf(file, len, "%s.%d", path, rank);
}

static void er_state_write(MPI_Comm comm_world, MPI_Comm comm_store, const char* path, int state)
{
  /* get our rank in our storage group */
  int rank_store;
  MPI_Comm_rank(comm_store, &rank_store);

  if (rank_store == 0) {
    /* build name of er state file */
    char er_file[1024];
    build_er_path(er_file, sizeof(er_file), path);

    /* write state to file */
    kvtree* data = kvtree_new();
    kvtree_util_set_int(data, "STATE", state);
    kvtree_write_file(er_file, data);
    kvtree_delete(&data);
  }

  /* wait for everyone to update state */
  MPI_Barrier(comm_world);

  return;
}

static void er_state_read(MPI_Comm comm_world, MPI_Comm comm_store, const char* path, int* state)
{
  /* intialize state to NULL */
  *state = ER_STATE_NULL;

  /* get our rank in our storage group */
  int rank_store;
  MPI_Comm_rank(comm_store, &rank_store);

  if (rank_store == 0) {
    /* build name of er state file */
    char er_file[1024];
    build_er_path(er_file, sizeof(er_file), path);

    /* read state from file */
    kvtree* data = kvtree_new();
    kvtree_read_file(er_file, data);
    kvtree_util_get_int(data, "STATE", state);
    kvtree_delete(&data);
  }

  /* TODO: we could end up with stale state files under cases like:
   * 1) job runs and writes file (file version 1), then dies
   * 2) job runs on different nodes and writes file (file version 2), then dies again
   * 3) job runs back on original nodes (some of which have v1 and some v2)
   *
   * right now, we're just picking the lowest rank that has some version */

  /* agree on state across processes,
   * just go with the lowest rank who has the value */
  int rank_world, ranks_world;
  MPI_Comm_rank(comm_world, &rank_world);
  MPI_Comm_size(comm_world, &ranks_world);

  /* if we have a state value, this process is eligible */
  int valid_rank = ranks_world;
  if (*state != ER_STATE_NULL) {
    valid_rank = rank_world;
  }

  /* compute lowest rank that has a valid value */
  int lowest_rank;
  MPI_Allreduce(&valid_rank, &lowest_rank, 1, MPI_INT, MPI_MIN, comm_world);

  /* get value from lowest rank with valid value,
   * if there was to valid value, state still be set to ER_STATE_NULL */
  if (lowest_rank < ranks_world) {
    MPI_Bcast(state, 1, MPI_INT, lowest_rank, comm_world);
  }

  return;
}

int ER_Init(const char* conf_file)
{
  int rc = ER_SUCCESS;

  /* allocate maps to track scheme and set data */
  er_schemes = kvtree_new();
  er_sets    = kvtree_new();

  /* initialize the redundancy library */
  if (redset_init() != REDSET_SUCCESS) {
    rc = ER_FAILURE;
  }

  /* initialize the shuffile library */
  if (shuffile_init() != SHUFFILE_SUCCESS) {
    rc = ER_FAILURE;
  }

  return rc;
}

int ER_Finalize()
{
  int rc = ER_SUCCESS;

  /* TODO: free descriptors for any outstanding schemes,
   * probably need to do this in same order on all procs,
   * for now, force user to clean up */
  kvtree* schemes = kvtree_get(er_schemes, "SCHEMES");
  if (kvtree_size(schemes) > 0) {
    er_err("ER_Finalize called before schemes freed");
    rc = ER_FAILURE;
  }

  /* free outstanding sets */
  kvtree* sets = kvtree_get(er_sets, "SETS");
  if (kvtree_size(sets) > 0) {
    er_err("ER_Finalize called before sets freed");
    rc = ER_FAILURE;
  }

  /* free maps */
  kvtree_delete(&er_schemes);
  kvtree_delete(&er_sets);

  /* shut down shuffile library */
  if (shuffile_finalize() != SHUFFILE_SUCCESS) {
    rc = ER_FAILURE;
  }

  /* shut down redundancy library */
  if (redset_finalize() != REDSET_SUCCESS) {
    rc = ER_FAILURE;
  }

  return rc;
}

int ER_Create_Scheme(
  MPI_Comm comm,
  const char* failure_domain,
  int encoding_blocks,
  int erasure_blocks)
{
  int rc = ER_SUCCESS;

  /* check that we can support the scheme the caller is asking for */
  int encoding_type = REDSET_COPY_NULL;
  if (encoding_blocks < 1) {
    /* no data to be encoded, don't know what to do */
    return ER_FAILURE;
  }
  if (erasure_blocks == 0) {
    encoding_type = REDSET_COPY_SINGLE;
  } else if (encoding_blocks == erasure_blocks) {
    encoding_type = REDSET_COPY_PARTNER;
  } else if (erasure_blocks == 1) {
    encoding_type = REDSET_COPY_XOR;
  } else {
    /* some form of Reed-Solomon that we don't support yet */
    return ER_FAILURE;
  }

  /* allocate a new redundancy descriptor */
  redset* d = ER_MALLOC(sizeof(redset));

  /* create the scheme */
  if (redset_create(encoding_type, comm, failure_domain, d) == REDSET_SUCCESS) {
    /* bump our internal counter */
    er_scheme_counter++;

    /* create a new record in our scheme map */
    kvtree* scheme = kvtree_set_kv_int(er_schemes, "SCHEMES", er_scheme_counter);

    /* record pointer to reddesc in our map */
    kvtree_util_set_ptr(scheme, "PTR", (void*)d);

    rc = ER_SUCCESS;
  } else {
    rc = ER_FAILURE;
  }

  /* return scheme id to caller, -1 if error */
  int ret = er_scheme_counter;
  if (rc != ER_SUCCESS) {
    ret = -1;
  }
  return ret;
}

int ER_Free_Scheme(int scheme_id)
{
  int rc = ER_SUCCESS;

  /* look up entry for this scheme id */
  kvtree* scheme = kvtree_get_kv_int(er_schemes, "SCHEMES", scheme_id);
  if (scheme) {
    /* get pointer to reddesc */
    redset* d;
    if (kvtree_util_get_ptr(scheme, "PTR", (void**)&d) == KVTREE_SUCCESS) {
      /* free reddesc */
      if (redset_delete(d) != REDSET_SUCCESS) {
        /* failed to free redundancy descriptor */
        rc = ER_FAILURE;
      }

      /* TODO: what to do here if redset_delete fails? */
      /* free the pointer to the redundancy descriptor */
      er_free(&d);
    } else {
      /* failed to find pointer to reddesc */
      rc = ER_FAILURE;
    }

    /* drop scheme entry from our map */
    kvtree_unset_kv_int(er_schemes, "SCHEMES", scheme_id);
  } else {
    /* failed to find scheme id in map */
    rc = ER_FAILURE;
  }

  return rc;
}

/* create a named set, and specify whether it should be encoded or recovered */
int ER_Create(MPI_Comm comm_world, MPI_Comm comm_store, const char* name, int direction, int scheme_id)
{
  int rc = ER_SUCCESS;

  /* check that we got a name */
  if (name == NULL || strcmp(name, "") == 0) {
    return ER_FAILURE;
  }

  /* check that we got a valid value for direction */
  if (direction != ER_DIRECTION_ENCODE  &&
      direction != ER_DIRECTION_REBUILD &&
      direction != ER_DIRECTION_REMOVE)
  {
    return ER_FAILURE;
  }

  /* bump set counter */
  er_set_counter++;

  /* record comms */
  erset* setptr = (erset*) ER_MALLOC(sizeof(erset));
  setptr->comm_world = comm_world;
  setptr->comm_store = comm_store;

  /* add an entry for this set */
  kvtree* set = kvtree_set_kv_int(er_sets, "SETS", er_set_counter);

  /* record operation direction */
  kvtree_util_set_str(set, "NAME", name);

  /* record operation direction */
  kvtree_util_set_int(set, "DIRECTION", direction);

  /* record pointer to our structure */
  kvtree_util_set_ptr(set, "STRUCT", setptr);

  /* when encoding, we need to remember the scheme,
   * it's implied by name on rebuild */
  if (direction == ER_DIRECTION_ENCODE) {
    /* record scheme id */
    kvtree_util_set_int(set, "SCHEME", scheme_id);

    /* look up entry for this scheme id */
    kvtree* scheme = kvtree_get_kv_int(er_schemes, "SCHEMES", scheme_id);
    if (! scheme) {
      /* failed to find scheme id in map */
      rc = ER_FAILURE;
    }
  }

  /* return set id to caller, -1 if error */
  int ret = er_set_counter;
  if (rc != ER_SUCCESS) {
    /* failed to create set, so delete it from our list */
    kvtree_unset_kv_int(er_sets, "SETS", er_set_counter);
    er_free(&setptr);
    ret = -1;
  }

  return ret;
}

/* adds file to specified set id */
int ER_Add(int set_id, const char* file)
{
  int rc = ER_SUCCESS;

  /* check that we got a file name */
  if (file == NULL || strcmp(file, "") == 0) {
    return ER_FAILURE;
  }

  /* lookup set id */
  kvtree* set = kvtree_get_kv_int(er_sets, "SETS", set_id);
  if (set) {
    /* add file to set */
    kvtree_set_kv(set, "FILE", file);

    /* TODO: capture current working dir? */
  } else {
    /* failed to find set id */
    rc = ER_FAILURE;
  }

  return rc;
}

static int er_encode(MPI_Comm comm_world, MPI_Comm comm_store, int num_files, const char** filenames, const char* path, redset d)
{
  int rc = ER_SUCCESS;

  int rank_world;
  MPI_Comm_rank(comm_world, &rank_world);

  int rank_store;
  MPI_Comm_rank(comm_store, &rank_store);

  /* build name of shuffile file */
  char shuffile_file[1024];
  build_shuffile_path(shuffile_file, sizeof(shuffile_file), path);

  /* TODO: read process name from scheme? */
  char redset_path[1024];
  build_redset_path(redset_path, sizeof(redset_path), path, rank_world);

  /* update data state to CORRUPT */
  er_state_write(comm_world, comm_store, path, ER_STATE_CORRUPT);

  /* apply redundancy */
  if (redset_apply(num_files, filenames, redset_path, d) != REDSET_SUCCESS) {
    /* failed to apply redundancy descriptor */
    rc = ER_FAILURE;
  }

  /* get list of files recording redudancy data */
  redset_filelist red_list = redset_filelist_get(redset_path, d);

  /* allocate space for a new file list to include both app files and redundancy files */
  int red_count = redset_filelist_count(red_list);
  int count = num_files + red_count;
  const char** filenames2 = (const char**) ER_MALLOC(count * sizeof(char*));

  /* fill in list of file names */
  int i;
  for (i = 0; i < num_files; i++) {
    /* application files */
    filenames2[i] = filenames[i];
  }
  for (i = 0; i < red_count; i++) {
    /* redundancy files */
    filenames2[num_files + i] = redset_filelist_file(red_list, i);
  }

  /* associate list of both app files and redundancy files with calling process */
  if (shuffile_create(comm_world, comm_store, count, filenames2, shuffile_file) != SHUFFILE_SUCCESS) {
    /* failed to register files with shuffile */
    rc = ER_FAILURE;
  }

  /* free the new file list */
  er_free(&filenames2);
  redset_filelist_release(&red_list);

  /* TODO: if successful, update state to ENCODED, otherwise CORRUPT */
  if (rc == ER_SUCCESS) {
    er_state_write(comm_world, comm_store, path, ER_STATE_ENCODED);
  }

  return rc;
}

static int er_rebuild(MPI_Comm comm_world, MPI_Comm comm_store, const char* path)
{
  int rc = ER_SUCCESS;

  /* read state file and ensure data is encoded */
  int state;
  er_state_read(comm_world, comm_store, path, &state);
  if (state != ER_STATE_ENCODED) {
    /* if it's not encoded, we can't attempt rebuild */
    return ER_FAILURE;
  }

  /* TODO: read process name from scheme? */
  int rank_world;
  MPI_Comm_rank(comm_world, &rank_world);

  /* build name of shuffile file */
  char shuffile_file[1024];
  build_shuffile_path(shuffile_file, sizeof(shuffile_file), path);

  /* build path to redset file for this process */
  char redset_path[1024];
  build_redset_path(redset_path, sizeof(redset_path), path, rank_world);

  /* update data state to CORRUPT */
  er_state_write(comm_world, comm_store, path, ER_STATE_CORRUPT);

  /* TODO: update state to SHUFFLE */

  /* migrate files back to ranks in case of new rank-to-node mapping */
  shuffile_migrate(comm_world, comm_store, shuffile_file);

  /* TODO: update state to RECOVER */

  /* rebuild files */
  redset d;
  if (redset_recover(comm_world, redset_path, &d) != REDSET_SUCCESS) {
    /* rebuild failed, rc is same value across comm_world */
    rc = ER_FAILURE;
  }

  /* if successful, update state to ENCODED, otherwise leave as CORRUPT */
  if (rc == ER_SUCCESS) {
    er_state_write(comm_world, comm_store, path, ER_STATE_ENCODED);
  }

  return rc;
}

static int er_remove(MPI_Comm comm_world, MPI_Comm comm_store, const char* path)
{
  int rc = ER_SUCCESS;

  /* get process name */
  int rank_world;
  MPI_Comm_rank(comm_world, &rank_world);

  /* get rank within storage group */
  int rank_store;
  MPI_Comm_rank(comm_store, &rank_store);

  /* build name of er state file */
  char er_file[1024];
  build_er_path(er_file, sizeof(er_file), path);

  /* build name of shuffile file */
  char shuffile_file[1024];
  build_shuffile_path(shuffile_file, sizeof(shuffile_file), path);

  /* build path to redset file for this process */
  char redset_path[1024];
  build_redset_path(redset_path, sizeof(redset_path), path, rank_world);

  /* update data state to CORRUPT */
  er_state_write(comm_world, comm_store, path, ER_STATE_CORRUPT);

  /* delete association information */
  shuffile_remove(comm_world, comm_store, shuffile_file);

  /* delete redundancy data, could avoid recover step if
   * the descriptor is cached somewhere */
  redset d;
  redset_recover(comm_world, redset_path, &d);
  redset_unapply(redset_path, d);
  redset_delete(&d);

  /* delete er state file */
  if (rank_store == 0) {
    unlink(er_file);
  }

  return rc;
}

/* initiate encode/rebuild operation on specified set id */
int ER_Dispatch(int set_id)
{
  int rc = ER_SUCCESS;

  /* lookup set id */
  kvtree* set = kvtree_get_kv_int(er_sets, "SETS", set_id);
  if (set) {
    /* get name of set */
    char* name = NULL;
    if (kvtree_util_get_str(set, "NAME", &name) != KVTREE_SUCCESS) {
      rc = ER_FAILURE;
    }

    /* TODO: allow caller to specify this prefix? */
    /* define prefix to use on all metadata files */
    char path[1024];
    snprintf(path, sizeof(path), "%s.er", name);

    /* get operation (encode, rebuild, remove) */
    int direction = 0;
    if (kvtree_util_get_int(set, "DIRECTION", &direction) != KVTREE_SUCCESS) {
      rc = ER_FAILURE;
    }

    /* get world and store communicators */
    MPI_Comm comm_world = MPI_COMM_NULL;
    MPI_Comm comm_store = MPI_COMM_NULL;
    erset* setptr;
    if (kvtree_util_get_ptr(set, "STRUCT", (void**)&setptr) == KVTREE_SUCCESS) {
      comm_world = setptr->comm_world;
      comm_store = setptr->comm_store;
    } else {
      rc = ER_FAILURE;
    }

    if (direction == ER_DIRECTION_ENCODE) {
      /* determine number of files */
      kvtree* files_hash = kvtree_get(set, "FILE");
      int num_files = kvtree_size(files_hash);

      /* allocate space for file names */
      const char** filenames = (const char**) ER_MALLOC(num_files * sizeof(char*));

      /* copy pointers to filenames */
      int i = 0;
      kvtree_elem* elem;
      for (elem = kvtree_elem_first(files_hash);
           elem != NULL;
           elem = kvtree_elem_next(elem))
      {
        const char* file = kvtree_elem_key(elem);
        filenames[i] = file;
        i++;
      }

      /* get scheme id */
      int scheme_id = 0;
      if (kvtree_util_get_int(set, "SCHEME", &scheme_id) != KVTREE_SUCCESS) {
        rc = ER_FAILURE;
      }

      /* get scheme */
      kvtree* scheme = kvtree_get_kv_int(er_schemes, "SCHEMES", scheme_id);
      if (scheme) {
        /* get redundancy descriptor from scheme */
        redset* dptr = NULL;
        if (kvtree_util_get_ptr(scheme, "PTR", (void**)&dptr) != KVTREE_SUCCESS) {
          rc = ER_FAILURE;
        }

        /* apply redundancy to files */
        if (rc == ER_SUCCESS) {
          rc = er_encode(comm_world, comm_store, num_files, filenames, path, *dptr);
        }
      } else {
        /* failed to find scheme id for this set */
        rc = ER_FAILURE;
      }

      /* free list of file names */
      er_free(&filenames);
    } else if (direction == ER_DIRECTION_REBUILD) {
      /* migrate files to new rank locations (if needed),
       * and rebuild missing files (if needed) */
      rc = er_rebuild(comm_world, comm_store, path);
    } else {
      /* delete metadata added when encoding files */
      rc = er_remove(comm_world, comm_store, path);
    }
  } else {
    /* failed to find set id */
    rc = ER_FAILURE;
  }

  return rc;
}

/* tests whether ongoing dispatch operation to finish,
 * returns 1 if done, 0 otherwise */
int ER_Test(int set_id)
{
  return 1;
}

/* wait for ongoing dispatch operation to finish */
int ER_Wait(int set_id)
{
  return ER_SUCCESS;
}

/* free internal resources associated with set id */
int ER_Free(int set_id)
{
  /* free resources associated with set */
  kvtree* set = kvtree_get_kv_int(er_sets, "SETS", set_id);
  if (set) {
    /* free struct attached to set, if any */
    erset* setptr;
    if (kvtree_util_get_ptr(set, "STRUCT", (void**)&setptr) == KVTREE_SUCCESS) {
      er_free(&setptr);
    }
  }

  /* delete the set from our list */
  kvtree_unset_kv_int(er_sets, "SETS", set_id);

  return ER_SUCCESS;
}