/////////////////////////////
////
////    NEW FEATURE: algo_gate
////
////    algos define targets for their common functions
////    and define a function for miner-thread to call to register
////    their targets. miner thread builds the gate, and array of structs
////    of function pointers, by calling each algo's register function.
//   Functions in this file are used simultaneously by myultiple
//   threads and must therefore be re-entrant.

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <memory.h>
#include <unistd.h>
#include "algo-gate-api.h"

// Define null and standard functions.
//
// Generic null functions do nothing except satisfy the syntax and
// can be used for optional safe gate functions.
//
// null gate functions are genarally used for mandatory and unsafe functions
// and will usually display an error massage and/or return a fail code.
// They are registered by default and are expected to be overwritten.
//
// std functions are non-null functions used by the most number of algos
// are are default.
//
// aux functions are functions used by many, but not most, algos and must
// be registered by eech algo using them. They usually have descriptive
// names.
//
// custom functions are algo spefic and are defined and registered in the
// algo's source file and are usually named [algo]_[function]. 
//
// In most cases the default is a null or std function. However in some
// cases, for convenience when the null function is not the most popular,
// the std function will be defined as default and the algo must register
// an appropriate null function.
//
// similar algos may share a gate function that may be defined here or
// in a source file common to the similar algos.
//
// gate functions may call other gate functions under the following
// restrictions. Any gate function defined here or used by more than one
// algo must call other functions using the gate: algo_gate.[function]. 
// custom functions may call other custom functions directly using
// [algo]_[function], howver it is recommended to alway use the gate.
//
// If, under rare circumstances, an algo with a custom gate function 
// needs to call a function of another algo it must define and register
// a private gate from its rgistration function and use it to call
// forein functions: [private_gate].[function]. If the algo needs to call
// a utility function defined here it may do so directly.
//
// The algo's gate registration function is caled once from the main thread
// and can do other intialization in addition such as setting options or
// other global or local (to the algo) variables.

// A set of predefined generic null functions that can be used as any null
// gate function with the same signature. 

void do_nothing   () {}
bool return_true  () { return true;  }
bool return_false () { return false; }
void *return_null () { return NULL;  }

void algo_not_tested()
{
  applog( LOG_WARNING,"Algo %s has not been tested live. It may not work",
          algo_names[opt_algo] );
  applog(LOG_WARNING,"and bad things may happen. Use at your own risk.");
}

void four_way_not_tested()
{
  applog( LOG_WARNING,"Algo %s has not been tested using 4way. It may not", algo_names[opt_algo] );
  applog( LOG_WARNING,"work or may be slower. Please report your results.");
}

void algo_not_implemented()
{
  applog(LOG_ERR,"Algo %s has not been Implemented.",algo_names[opt_algo]);
}

// default null functions
// deprecated, use generic as default
int null_scanhash()
{
   applog(LOG_WARNING,"SWERR: undefined scanhash function in algo_gate");
   return 0;
}

// Default generic scanhash can be used in many cases. Not to be used when
// prehashing can be done or when byte swapping the data can be avoided.
int scanhash_generic( struct work *work, uint32_t max_nonce,
                      uint64_t *hashes_done, struct thr_info *mythr )
{
   uint32_t edata[20] __attribute__((aligned(64)));
   uint32_t hash[8] __attribute__((aligned(64)));
   uint32_t *pdata = work->data;
   uint32_t *ptarget = work->target;
   const uint32_t first_nonce = pdata[19];
   const uint32_t last_nonce = max_nonce - 1;
   uint32_t n = first_nonce;
   const int thr_id = mythr->id;
   const bool bench = opt_benchmark;

   v128_bswap32_80( edata, pdata );
   do
   {
      edata[19] = n;
      if ( likely( algo_gate.hash( hash, edata, thr_id ) ) )
      if ( unlikely( valid_hash( hash, ptarget ) && !bench ) )
      {
         pdata[19] = bswap_32( n );
         submit_solution( work, hash, mythr );
      }
      n++;
   } while ( n < last_nonce && !work_restart[thr_id].restart );
   *hashes_done = n - first_nonce;
   pdata[19] = n;
   return 0;
}

#if defined(__AVX2__)

//int scanhash_4way_64_64( struct work *work, uint32_t max_nonce,
//                      uint64_t *hashes_done, struct thr_info *mythr )

//int scanhash_4way_64_640( struct work *work, uint32_t max_nonce,
//                      uint64_t *hashes_done, struct thr_info *mythr )

int scanhash_4way_64in_32out( struct work *work, uint32_t max_nonce,
                      uint64_t *hashes_done, struct thr_info *mythr )
{
   uint32_t hash32[8*4] __attribute__ ((aligned (64)));
   uint32_t vdata[20*4] __attribute__ ((aligned (64)));
   uint32_t lane_hash[8] __attribute__ ((aligned (64)));
   uint32_t *hash32_d7 = &(hash32[ 7*4 ]);
   uint32_t *pdata = work->data;
   const uint32_t *ptarget = work->target;
   const uint32_t first_nonce = pdata[19];
   const uint32_t last_nonce = max_nonce - 4;
   __m256i  *noncev = (__m256i*)vdata + 9;
   uint32_t n = first_nonce;
   const int thr_id = mythr->id;
   const uint32_t targ32_d7 = ptarget[7];
   const bool bench = opt_benchmark;

   mm256_bswap32_intrlv80_4x64( vdata, pdata );
   // overwrite byte swapped nonce with original byte order for proper
   // incrementing. The nonce only needs to byte swapped if it is to be
   // sumbitted.
   *noncev = mm256_intrlv_blend_32(
                   _mm256_set_epi32( n+3, 0, n+2, 0, n+1, 0, n, 0 ), *noncev );
   do
   {
      if ( likely( algo_gate.hash( hash32, vdata, thr_id ) ) )
      for ( int lane = 0; lane < 4; lane++ )
      if ( unlikely( hash32_d7[ lane ] <= targ32_d7 && !bench ) )
      {
         extr_lane_4x32( lane_hash, hash32, lane, 256 );
         if ( valid_hash( lane_hash, ptarget ) )
         {
            pdata[19] = bswap_32( n + lane );
            submit_solution( work, lane_hash, mythr );
         }
      }
      *noncev = _mm256_add_epi32( *noncev,
                                  _mm256_set1_epi64x( 0x0000000400000000 ) );
      n += 4;
   } while ( likely( ( n <= last_nonce ) && !work_restart[thr_id].restart ) );
   pdata[19] = n;
   *hashes_done = n - first_nonce;
   return 0;
}

//int scanhash_8way_32_32( struct work *work, uint32_t max_nonce,
//                      uint64_t *hashes_done, struct thr_info *mythr )

#endif

#if defined(SIMD512)

//int scanhash_8way_64_64( struct work *work, uint32_t max_nonce,
//                      uint64_t *hashes_done, struct thr_info *mythr )

//int scanhash_8way_64_640( struct work *work, uint32_t max_nonce,
//                      uint64_t *hashes_done, struct thr_info *mythr )

int scanhash_8way_64in_32out( struct work *work, uint32_t max_nonce,
                      uint64_t *hashes_done, struct thr_info *mythr )
{
   uint32_t hash32[8*8] __attribute__ ((aligned (128)));
   uint32_t vdata[20*8] __attribute__ ((aligned (64)));
   uint32_t lane_hash[8] __attribute__ ((aligned (64)));
   uint32_t *hash32_d7 = &(hash32[7*8]);
   uint32_t *pdata = work->data;
   const uint32_t *ptarget = work->target;
   const uint32_t first_nonce = pdata[19];
   const uint32_t last_nonce = max_nonce - 8;
   __m512i  *noncev = (__m512i*)vdata + 9;
   uint32_t n = first_nonce;
   const int thr_id = mythr->id;
   const uint32_t targ32_d7 = ptarget[7];
   const bool bench = opt_benchmark;

   mm512_bswap32_intrlv80_8x64( vdata, pdata );
   *noncev = mm512_intrlv_blend_32(
              _mm512_set_epi32( n+7, 0, n+6, 0, n+5, 0, n+4, 0,
                                n+3, 0, n+2, 0, n+1, 0, n,   0 ), *noncev );
   do
   {
      if ( likely( algo_gate.hash( hash32, vdata, thr_id ) ) )
      for ( int lane = 0; lane < 8; lane++ )
      if ( unlikely( ( hash32_d7[ lane ] <= targ32_d7 ) && !bench ) )
      {
         extr_lane_8x32( lane_hash, hash32, lane, 256 );
         if ( likely( valid_hash( lane_hash, ptarget ) ) )
         {
            pdata[19] = bswap_32( n + lane );
            submit_solution( work, lane_hash, mythr );
         }
      }
      *noncev = _mm512_add_epi32( *noncev,
                                  _mm512_set1_epi64( 0x0000000800000000 ) );
      n += 8;
   } while ( likely( ( n < last_nonce ) && !work_restart[thr_id].restart ) );
   pdata[19] = n;
   *hashes_done = n - first_nonce;
   return 0;
}

//int scanhash_16way_32_32( struct work *work, uint32_t max_nonce,
//                      uint64_t *hashes_done, struct thr_info *mythr )

#endif



int null_hash()
{
   applog(LOG_WARNING,"SWERR: null_hash unsafe null function");
   return 0;
};

static void init_algo_gate( algo_gate_t* gate )
{
   gate->miner_thread_init       = (void*)&return_true;
   gate->scanhash                = (void*)&scanhash_generic;
   gate->hash                    = (void*)&null_hash;
   gate->get_new_work            = (void*)&std_get_new_work;
   gate->work_decode             = (void*)&std_le_work_decode;
   gate->decode_extra_data       = (void*)&do_nothing;
   gate->gen_merkle_root         = (void*)&sha256d_gen_merkle_root;
   gate->build_stratum_request   = (void*)&std_le_build_stratum_request;
   gate->malloc_txs_request      = (void*)&std_malloc_txs_request;
   gate->submit_getwork_result   = (void*)&std_le_submit_getwork_result;
   gate->build_block_header      = (void*)&std_build_block_header;
   gate->build_extraheader       = (void*)&std_build_extraheader;
   gate->set_work_data_endian    = (void*)&do_nothing;
   gate->resync_threads          = (void*)&do_nothing;
   gate->do_this_thread          = (void*)&return_true;
   gate->longpoll_rpc_call       = (void*)&std_longpoll_rpc_call;
   gate->get_work_data_size      = (void*)&std_get_work_data_size;
   gate->optimizations           = EMPTY_SET;
   gate->ntime_index             = STD_NTIME_INDEX;
   gate->nbits_index             = STD_NBITS_INDEX;
   gate->nonce_index             = STD_NONCE_INDEX;
   gate->work_cmp_size           = STD_WORK_CMP_SIZE;
}

// Ignore warnings for not yet defined register functions
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"

// Called once by main
bool register_algo_gate( int algo, algo_gate_t *gate )
{
  bool rc = false;

  if ( NULL == gate )
  {
    applog(LOG_ERR,"FAIL: algo_gate registration failed, NULL gate\n");
    return false;
  }

  init_algo_gate( gate );

  switch ( algo )
  {
    case ALGO_KECCAK:       rc = register_keccak_algo        ( gate ); break;
    case ALGO_KECCAKC:      rc = register_keccakc_algo       ( gate ); break;
    case ALGO_QHASH:        rc = register_qhash_algo         ( gate ); break;
    case ALGO_SHA256D:      rc = register_sha256d_algo       ( gate ); break;
    case ALGO_SHA256DT:     rc = register_sha256dt_algo      ( gate ); break;
    case ALGO_SHA256Q:      rc = register_sha256q_algo       ( gate ); break;
    case ALGO_SHA256T:      rc = register_sha256t_algo       ( gate ); break;
    case ALGO_SHA3D:        rc = register_sha3d_algo         ( gate ); break;
    case ALGO_SHA512256D:   rc = register_sha512256d_algo    ( gate ); break;
   default:
      applog(LOG_ERR,"BUG: unregistered algorithm %s.\n", algo_names[opt_algo] );
      return false;
  } // switch

  if ( !rc )
  {
    applog(LOG_ERR, "FAIL: %s algorithm failed to initialize\n", algo_names[opt_algo] );
    return false;
  }
  return true;
}

// restore warnings
#pragma GCC diagnostic pop

void exec_hash_function( int algo, void *output, const void *pdata )
{
  algo_gate_t gate;   
  gate.hash = (void*)&null_hash;
  register_algo_gate( algo, &gate );
  gate.hash( output, pdata, 0 );  
}

#define PROPER (1)
#define ALIAS  (0)

// The only difference between the alias and the proper algo name is the
// proper name is the one that is defined in ALGO_NAMES. There may be
// multiple aliases that map to the same proper name.
// New aliases can be added anywhere in the array as long as NULL is last.
// Alphabetic order of alias is recommended.
const char* const algo_alias_map[][2] =
{
//   alias                proper
  { NULL,                NULL             }   
};

// if arg is a valid alias for a known algo it is updated with the proper
// name. No validation of the algo or alias is done, It is the responsinility
// of the calling function to validate the algo after return.
void get_algo_alias( char** algo_or_alias )
{
  int i;
  for ( i=0; algo_alias_map[i][ALIAS]; i++ )
    if ( !strcasecmp( *algo_or_alias, algo_alias_map[i][ ALIAS ] ) )
    {
      // found valid alias, return proper name
      *algo_or_alias = (char*)( algo_alias_map[i][ PROPER ] );
      return;
    }
}

#undef ALIAS
#undef PROPER

