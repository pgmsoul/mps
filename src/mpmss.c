/*  impl.c.mpmss: MPM STRESS TEST
 *
 *  $HopeName: !mpmss.c(MM_dylan_honeybee.1) $
 * Copyright (C) 1997 The Harlequin Group Limited.  All rights reserved.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

#include "mps.h"
#include "mpscmv.h"
#include "mpslib.h"
#include "testlib.h"
#ifdef MPS_OS_SU
#include "ossu.h"
#endif


/* @@@@ Hack due to missing mpscmfs.h */
extern mps_class_t PoolClassMFS(void);


#define TEST_SET_SIZE 200


static mps_res_t stress(mps_class_t class, mps_space_t space, size_t (*size)(int i), ...)
{
  mps_res_t res;
  mps_pool_t pool;
  va_list arg;
  int i;
  void *ps[TEST_SET_SIZE];
  size_t ss[TEST_SET_SIZE];

  va_start(arg, size);
  res = mps_pool_create_v(&pool, space, class, arg);
  va_end(arg);
  if(res != MPS_RES_OK) return res;

  for(i=0; i<TEST_SET_SIZE; ++i)
  {
    ss[i] = (*size)(i);

    res = mps_alloc((mps_addr_t *)&ps[i], pool, ss[i]);
    if(res != MPS_RES_OK) return res;

    if(i && i%4==0) putchar('\n');
    printf("%8lX %6lX ", (unsigned long)ps[i], (unsigned long)ss[i]);
  }
  putchar('\n');

  for(i=0; i<TEST_SET_SIZE; ++i)
  {
    int j = rand()%(TEST_SET_SIZE-i);
    void *tp;
    size_t ts;

    tp = ps[j]; ts = ss[j];
    ps[j] = ps[i]; ss[j] = ss[i];
    ps[i] = tp; ss[i] = ts;
  }

  for(i=0; i<TEST_SET_SIZE; ++i)
  {
    mps_free(pool, (mps_addr_t)ps[i], ss[i]);
/*    if(i == TEST_SET_SIZE/2)
      PoolDescribe((Pool)pool, mps_lib_stdout); */
  }

  mps_pool_destroy(pool);

  return MPS_RES_OK;
}


#define max(a, b) (((a) > (b)) ? (a) : (b))


static size_t randomSize(int i)
{
  /* Make the range large enough to span three pages in the segment table: */
  /* 160 segments/page, page size max 0x2000. */
  size_t maxSize = 2 * 160 * 0x2000;
  /* Reduce by a factor of 2 every 10 cycles.  Total allocation about 40 MB. */
  return rnd() % max((maxSize >> (i / 10)), 2) + 1;
}


static size_t fixedSizeSize = 0;

static size_t fixedSize(int i)
{
  (void)i;
  return fixedSizeSize;
}


int main(void)
{
  mps_space_t space;

  srand(time(NULL));

  die(mps_space_create(&space), "SpaceInit");

  fixedSizeSize = 13;
  die(stress(PoolClassMFS(),
             space, fixedSize, (size_t)100000, fixedSizeSize),
      "stress MFS");

  die(stress(mps_class_mv(),
             space, randomSize, (size_t)65536,
             (size_t)32, (size_t)65536), "stress MV");

  mps_space_destroy(space);

  return 0;
}
