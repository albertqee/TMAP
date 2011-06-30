/* Copyright (C) 2010 Ion Torrent Systems, Inc. All Rights Reserved */
/* The MIT License

   Copyright (c) 2011 by Attractive Chaos <attractor@live.co.uk>

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
   */

#include <stdlib.h>
#include <stdint.h>
#include <emmintrin.h>
#include <smmintrin.h>
#include <unistd.h>
#include <stdio.h>
#include "../util/tmap_error.h"
#include "../util/tmap_alloc.h"
#include "../util/tmap_definitions.h"
#include "tmap_sw.h"
#include "tmap_vsw.h"
#include "tmap_vsw16.h"

/*
static void
tmap_vsw16_query_print_query_profile(tmap_vsw16_query_t *query)
{
  int32_t a, i, j;

  for(a=0;a<TMAP_VSW_ALPHABET_SIZE;a++) {
      fprintf(stderr, "a=%d ", a);
      __m128i *S = query->query_profile + a * query->slen;
      for(i = 0; i < tmap_vsw16_values_per_128_bits; i++) {
          for(j = 0; j < query->slen; j++) {
              fprintf(stderr, " %d", ((tmap_vsw16_int_t*)(S+j))[i]);
          }
      }
      fprintf(stderr, "\n");
  }
}
*/

tmap_vsw16_query_t *
tmap_vsw16_query_init(tmap_vsw16_query_t *prev, const uint8_t *query, int32_t qlen, int32_t tlen, 
                              int32_t query_start_clip, int32_t query_end_clip,
                              int32_t type, tmap_vsw_opt_t *opt)
{
  int32_t qlen_mem, slen, a;
  tmap_vsw16_int_t *t;
  int32_t hlen = 0, hlen_mem = 0;

  // get the number of stripes
  slen = __tmap_vsw16_calc_slen(qlen); 
  if(1 == type) {
      hlen = qlen * tlen;
      hlen_mem = __tmap_vsw_calc_qlen_mem(hlen); // same calculation
  }

  // check if we need to re-size the memory
  if(NULL == prev 
     || (0 == type && prev->qlen_max < qlen) 
     || (1 == type && prev->hlen_mem < hlen_mem) 
     || type != prev->type
     || query_start_clip != prev->query_start_clip
     || query_end_clip != prev->query_end_clip) { // recompute
      // free previous
      if(NULL != prev) {
          free(prev); prev = NULL;
      }
      // get the memory needed to hold the stripes for the query 
      qlen_mem = __tmap_vsw_calc_qlen_mem(slen);
      if(0 == type) {
          prev = tmap_malloc(sizeof(tmap_vsw16_query_t) + 15 + qlen_mem * (TMAP_VSW_ALPHABET_SIZE + 3), "prev"); // add three for H0, H1, and E
          // update the type
          prev->type = 0;
          prev->qlen_max = qlen; 
      }
      else if(1 == type) {
          // allocate a single block of memory (add 15 for 16-byte aligning the prev
          // pointer): struct + (16-byte alignment) + query profile + scoring matrix 
          prev = tmap_malloc(sizeof(tmap_vsw16_query_t) + 15 + (qlen_mem * TMAP_VSW_ALPHABET_SIZE + 3) + (hlen_mem), "prev"); 
          // update the type
          prev->type = 1;
          prev->hlen_mem = hlen_mem;
          prev->hlen_max = hlen; 
      }
      else {
          tmap_error("bug encountered", Exit, OutOfRange);
      }
      // update the memory
      prev->qlen_mem = qlen_mem;
      // update clipping
      prev->query_start_clip = query_start_clip;
      prev->query_end_clip = query_end_clip;
  }
  else if(((0 == type && prev->qlen ==qlen) || (1 == type && prev->hlen_mem < hlen_mem))
          && type == prev->type
          && query_start_clip == prev->query_start_clip
          && query_end_clip == prev->query_end_clip) { // no need to recompute query profile etc...
      return prev;
  }

  // compute min/max edit scores
  prev->min_edit_score = (opt->pen_gapo+opt->pen_gape < opt->pen_mm) ? -opt->pen_mm : -(opt->pen_gapo+opt->pen_gape); // minimum single-edit score
  prev->min_edit_score--; // for N mismatches
  prev->max_edit_score = opt->score_match;
  // compute the zero alignment scores
  if(0 == query_start_clip // leading insertions/mismatches allowed in the alignment
     || (0 == query_end_clip)) { // trailing insertions/mismatches allowed in the alignment
          int32_t min_gap_score, min_mm_score;
          min_mm_score = qlen * -opt->pen_mm; // all mismatches
          min_gap_score = -opt->pen_gapo + (-opt->pen_gape * qlen); // all insertions
          prev->zero_aln_score = (min_mm_score < min_gap_score) ? min_mm_score : min_gap_score;
  }
  else { // can clip ant least one of {prefix,suffix} of the query
      prev->zero_aln_score = prev->min_edit_score;
  }
  // the minimum alignment score
  prev->min_aln_score = prev->zero_aln_score << 1; // double it so that we have room in case of starting at negative infinity
  prev->min_aln_score += prev->min_edit_score; // so it doesn't underflow
  // max aln score
  prev->max_aln_score = tmap_vsw16_max_value - prev->max_edit_score; // so it doesn't overflow
  // normalize
  prev->zero_aln_score = tmap_vsw16_min_value - prev->min_aln_score- prev->zero_aln_score; 
  prev->min_aln_score = tmap_vsw16_min_value - prev->min_aln_score; 
  // normalize with the minimum alignment score
  /*
  fprintf(stderr, "min_edit_score=%d\n", prev->min_edit_score);
  fprintf(stderr, "max_edit_score=%d\n", prev->max_edit_score);
  fprintf(stderr, "zero_aln_score=%d\n", prev->zero_aln_score);
  fprintf(stderr, "min_aln_score=%d\n", prev->min_aln_score);
  fprintf(stderr, "max_aln_score=%d\n", prev->max_aln_score);
  */

  prev->qlen = qlen; // update the query length
  prev->hlen = hlen; // the scoring matrix size 
  prev->slen = slen; // update the number of stripes

  // NB: align all the memory from one block
  prev->query_profile = (__m128i*)__tmap_vsw_16((size_t)prev + sizeof(tmap_vsw16_query_t)); // skip over the struct variables, align memory 
  prev->H0 = prev->query_profile + (prev->slen * TMAP_VSW_ALPHABET_SIZE); // skip over the query profile
  prev->H1 = prev->H0 + prev->slen; // skip over H0
  prev->E = prev->H1 + prev->slen; // skip over H1
  if(1 == type) {
      prev->H = prev->E + prev->slen; // skip over E
  }
  
  // create the query profile
  t = (tmap_vsw16_int_t*)prev->query_profile;
  for(a = 0; a < TMAP_VSW_ALPHABET_SIZE; a++) {
      int32_t i, k;
      for(i = 0; i < prev->slen; ++i) { // for each stripe
          // fill in this stripe
          for(k = i; k < prev->slen << tmap_vsw16_values_per_128_bits_log2; k += prev->slen) { //  for q_{i+1}, q_{2i+1}, ..., q_{2s+1}
              // NB: pad with zeros
              *t++ = ((k >= qlen) ? prev->min_edit_score : ((a == query[k]) ? opt->score_match : -opt->pen_mm)); 
          }
      }
  }
  //tmap_vsw16_query_print_query_profile(prev);
  return prev;
}

void
tmap_vsw16_query_reinit(tmap_vsw16_query_t *query, tmap_vsw_result_t *result)
{
  tmap_vsw16_int_t *new, *old;
  int32_t a, i, k;
  int32_t slen;

  if(0 <= result->query_end && result->query_end+1 < query->qlen) {
      // update the query profile
      slen = __tmap_vsw16_calc_slen(query->qlen);
      new = old = (tmap_vsw16_int_t*)query->query_profile;
      for(a = 0; a < TMAP_VSW_ALPHABET_SIZE; a++) {
          for(i = 0; i < slen; ++i) { // for each new stripe
              for(k = i; k < slen << tmap_vsw16_values_per_128_bits_log2; k += slen) { //  for q_{i+1}, q_{2i+1}, ..., q_{2s+1}
                  if(query->qlen <= k) {
                      *new = query->min_edit_score;
                  }
                  else {
                      // use the old query profile
                      *new = *old;
                  }
                  new++;
                  old++;
              }
          }
          // skip over old stripes
          for(i = slen; i < query->slen; i++) { // for each old stripe (to skip)
              for(k = i; k < query->slen << tmap_vsw16_values_per_128_bits_log2; k += query->slen) { //  for q_{i+1}, q_{2i+1}, ..., q_{2s+1}
                  old++;
              }
          }
      }
      // update variables
      query->qlen = result->query_end+1;
      query->slen = slen;
      // update H0, H1, and E pointers
      query->H0 = query->query_profile + (query->slen * TMAP_VSW_ALPHABET_SIZE); // skip over the query profile
      query->H1 = query->H0 + query->slen; // skip over H0
      query->E = query->H1 + query->slen; // skip over H1
      if(1 == query->type) {
          // update H
          query->H = query->E + query->slen; // skip over E
      }
  }
}

void
tmap_vsw16_query_destroy(tmap_vsw16_query_t *vsw)
{
  // all memory was allocated as one block
  free(vsw);
}

int32_t
tmap_vsw16_sse2_forward(tmap_vsw16_query_t *query, const uint8_t *target, int32_t tlen, 
                        int32_t query_start_skip, int32_t target_start_skip,
                        int32_t query_start_clip, int32_t query_end_clip,
                        tmap_vsw_opt_t *opt, int32_t *query_end, int32_t *target_end,
                        int32_t direction, int32_t *overflow)
{
  int32_t slen, i, j, k, imax = 0, imin = 0, sum = 0;
  uint16_t cmp;
  int32_t gmax, best, zero;
  int32_t query_start_skip_j = 0, query_start_skip_k = 0;
  __m128i zero_mm, negative_infinity_mm, positive_infinity_mm, reduce_mm;
  __m128i pen_gapoe, pen_gape, *H0, *H1, *E, *H, init_gape;

  // initialization
  // normalize these
  zero = query->zero_aln_score; // where the normalized zero alignment score occurs
  negative_infinity_mm = __tmap_vsw16_mm_set1_epi16(query->min_aln_score); // the minimum possible value
  positive_infinity_mm = __tmap_vsw16_mm_set1_epi16(query->max_aln_score); // the minimum possible value
  // these are not normalized
  pen_gapoe = __tmap_vsw16_mm_set1_epi16(opt->pen_gapo + opt->pen_gape); // gap open penalty
  pen_gape = __tmap_vsw16_mm_set1_epi16(opt->pen_gape); // gap extend penalty
  zero_mm = __tmap_vsw16_mm_set1_epi16(zero); // where the normalized zero alignment score occurs
  gmax = tmap_vsw16_min_value;
  best = tmap_vsw16_min_value;
  reduce_mm = __tmap_vsw16_mm_set1_epi16(tmap_vsw16_mid_value);
  init_gape = __tmap_vsw16_mm_set1_epi16((tmap_vsw16_int_t)(-opt->pen_gape));
  // vectors
  H0 = query->H0; 
  H1 = query->H1; 
  E = query->E; 
  H = query->H;
  slen = query->slen;
  if(NULL != overflow) *overflow = 0;

  /*
  fprintf(stderr, "query_start_skip=%d target_start_skip=%d\n",
          query_start_skip, target_start_skip);
          */

  // select query end only
  // check stripe #: __tmap_vsw16_query_index_to_stripe_number(query->qlen, slen) 
  // check byte # __tmap_vsw16_query_index_to_byte_number(query->qlen, slen)

  if(0 == query_start_clip) {
      __m128i e, s, g, *S;
      // Set start
      for(j = 0; j < slen; j++) {
          // initialize H0 to negative infinity 
          __tmap_vsw_mm_store_si128(H0 + j, negative_infinity_mm); // H(0,j)
          __tmap_vsw_mm_store_si128(E + j, negative_infinity_mm); // H(0,j)
      }
      // NB: setting the start == 0 will be done later
      
      // NB: we will set the leading insertions later
      S = query->query_profile + target[0] * slen; // s is the 1st score vector
      s = __tmap_vsw_mm_load_si128(S + slen - 1);
      s = __tmap_vsw_mm_slli_si128(s, tmap_vsw16_shift_bytes); // shift left since x64 is little endian
      for(j = 0; j < slen; j++) {
          // gap open and gap extension
          e = __tmap_vsw16_mm_subs_epi16(zero_mm, pen_gapoe);
          // add the score vector
          e = __tmap_vsw16_mm_adds_epi16(e, s); 
          // gap extensions
          g = pen_gape;
          if(1 < j) g = __tmap_vsw_mm_slli_si128(g, tmap_vsw16_shift_bytes);
          e = __tmap_vsw16_mm_subs_epi16(e, g); 
          // store
          __tmap_vsw_mm_store_si128(E + j, e);
      }
  }
  else {
      // Initialize all zeros
      for(i = 0; i < slen; ++i) {
          __tmap_vsw_mm_store_si128(E + i, zero_mm);
          __tmap_vsw_mm_store_si128(H0 + i, zero_mm);
      }
  }

  // Debugging
  // H0
  /*
  fprintf(stderr, "i=-1");
  for(j = 0; j < tmap_vsw16_values_per_128_bits; j++) { // for each start position in the stripe
      for(k = 0; k < query->slen; k++) {
          fprintf(stderr, " %d", ((tmap_vsw16_int_t*)(E+k))[j] - zero);
      }
  }
  fprintf(stderr, "\n");
  */
  
  query_start_skip_j = query_start_skip % slen; // stripe
  query_start_skip_k = query_start_skip / slen; // byte
  //fprintf(stderr, "query_start_skip_j=%d query_start_skip_k=%d\n", query_start_skip_j, query_start_skip_k);

  // the core loop
  for(i = 0, sum = 0; i < tlen; i++) { // for each base in the target
      __m128i e, h, f, max, min, *S;

      max = negative_infinity_mm; // max is negative infinity
      min = positive_infinity_mm; // min is positive infinity
      S = query->query_profile + target[i] * slen; // s is the 1st score vector

      // load H(i-1,-1)
      h = __tmap_vsw_mm_load_si128(H0 + slen - 1); // the last stripe, which holds the j-1 
      h = __tmap_vsw_mm_slli_si128(h, tmap_vsw16_shift_bytes); // shift left since x64 is little endian
      if(1 == query_start_clip || 0 == i) {
          h = __tmap_vsw16_mm_insert_epi16(h, zero, 0); // since we shifted in zeros
      }
      else {
          h = __tmap_vsw16_mm_insert_epi16(h, query->min_aln_score, 0); // since we shifted in zeros
      }

      // we will do a lazy-F loop, so use negative infinity
      f = negative_infinity_mm;

      for(j = 0; TMAP_VSW_LIKELY(j < slen); ++j) { // for each stripe in the query
          // NB: at the beginning, 
          // h=H(i-1,j-1)
          // f=H(i,j)
          /* SW cells are computed in the following order:
           *   H(i,j)   = max{H(i-1,j-1)+S(i,j), E(i,j), F(i,j)}
           *   E(i+1,j) = max{H(i,j)-q, E(i,j)-r}
           *   F(i,j+1) = max{H(i,j)-q, F(i,j)-r}
           */
          if(1 == query_start_clip) {
              // start anywhere within the query
              h = __tmap_vsw16_mm_max_epi16(h, zero_mm);  
          }
          else if(j == query_start_skip_j) { // set the start
              h = __tmap_vsw16_mm_insert(h, zero, query_start_skip_k);
          }
          // compute H(i,j); 
          /*
          __m128i s = __tmap_vsw_mm_load_si128(S + j);
          fprintf(stderr, "s i=%d j=%d", i, j);
          for(k = 0; k < tmap_vsw16_values_per_128_bits; k++) { // for each start position in the stripe
              fprintf(stderr, " %d", ((tmap_vsw16_int_t*)(&s))[k]);
          }
          fprintf(stderr, "\n");
          fprintf(stderr, "h i=%d j=%d", i, j);
          for(k = 0; k < tmap_vsw16_values_per_128_bits; k++) { // for each start position in the stripe
              fprintf(stderr, " %d", ((tmap_vsw16_int_t*)(&h))[k] - zero);
          }
          fprintf(stderr, "\n");
          */
          h = __tmap_vsw16_mm_adds_epi16(h, __tmap_vsw_mm_load_si128(S + j)); // h=H(i-1,j-1)+S(i,j)
          /*
          fprintf(stderr, "h' i=%d j=%d", i, j);
          for(k = 0; k < tmap_vsw16_values_per_128_bits; k++) { // for each start position in the stripe
              fprintf(stderr, " %d", ((tmap_vsw16_int_t*)(&h))[k] - zero);
          }
          fprintf(stderr, "\n");
          */
          e = __tmap_vsw_mm_load_si128(E + j); // e=E(i,j)
          h = __tmap_vsw16_mm_max_epi16(h, e); // h=H(i,j) = max{E(i,j), H(i-1,j-1)+S(i,j)}
          h = __tmap_vsw16_mm_max_epi16(h, f); // h=H(i,j) = max{max{E(i,j), H(i-1,j-1)+S(i,j)}, F(i,j)}
          h = __tmap_vsw16_mm_max_epi16(h, negative_infinity_mm); // bound with -inf
          max = __tmap_vsw16_mm_max_epi16(max, h); // save the max values in this stripe versus the last
          min = __tmap_vsw16_mm_min_epi16(min, h); // save the max values in this stripe versus the last
          __tmap_vsw_mm_store_si128(H1 + j, h); // save h to H(i,j)
          // next, compute E(i+1,j)
          h = __tmap_vsw16_mm_subs_epi16(h, pen_gapoe); // h=H(i,j)-pen_gapo
          e = __tmap_vsw16_mm_subs_epi16(e, pen_gape); // e=E(i,j)-pen_gape
          e = __tmap_vsw16_mm_max_epi16(e, h); // e=E(i+1,j) = max{E(i,j)-pen_gape, H(i,j)-pen_gapo}
          e = __tmap_vsw16_mm_max_epi16(e, negative_infinity_mm); // bound with -inf
          __tmap_vsw_mm_store_si128(E + j, e); // save e to E(i+1,j)
          // now compute F(i,j+1)
          //h = __tmap_vsw16_mm_subs_epi16(h, pen_gapoe); // h=H(i,j)-pen_gapo
          f = __tmap_vsw16_mm_subs_epi16(f, pen_gape); // f=F(i,j)-pen_gape
          f = __tmap_vsw16_mm_max_epi16(f, h); // f=F(i,j+1) = max{F(i,j)-pen_gape, H(i,j)-pen_gapo}
          f = __tmap_vsw16_mm_max_epi16(f, negative_infinity_mm); // bound with -inf
          // get H(i-1,j) and prepare for the next j
          h = __tmap_vsw_mm_load_si128(H0 + j); // h=H(i-1,j)
      }
      // NB: we do not need to set E(i,j) as we disallow adjacent insertion and then deletion
      // iterate through each value stored in a stripe
      for(k = 0; TMAP_VSW_LIKELY(k < tmap_vsw16_values_per_128_bits); ++k) { // this block mimics SWPS3; NB: H(i,j) updated in the lazy-F loop cannot exceed max
          f = __tmap_vsw_mm_slli_si128(f, tmap_vsw16_shift_bytes); // since x86 is little endian
          f = __tmap_vsw16_mm_insert_epi16(f, query->min_aln_score, 0); // set F(i-1,-1)[0] as negative infinity (normalized)
          for(j = 0; TMAP_VSW_LIKELY(j < slen); ++j) { // we require at most 'slen' iterations to guarantee F propagation
              h = __tmap_vsw_mm_load_si128(H1 + j); // h=H(i,j)
              h = __tmap_vsw16_mm_max_epi16(h, f); // h=H(i,j) = max{H(i,j), F(i,j)}
              h = __tmap_vsw16_mm_max_epi16(h, negative_infinity_mm); // bound with -inf
              __tmap_vsw_mm_store_si128(H1 + j, h); // save h to H(i,j) 
              h = __tmap_vsw16_mm_subs_epi16(h, pen_gapoe); // h=H(i,j)-gapo
              f = __tmap_vsw16_mm_subs_epi16(f, pen_gape); // f=max{F(i,j+)-pen_gape,H(i,j)-pen_gapo}
              f = __tmap_vsw16_mm_max_epi16(f, negative_infinity_mm); // bound with -inf
              // check to see if h could have been updated by f?
              // NB: the comparison below will have some false positives, in
              // other words h == f a priori, but this is rare.
              cmp = __tmap_vsw16_mm_movemask_epi16(__tmap_vsw16_mm_cmpeq_epi16(__tmap_vsw16_mm_subs_epi16(f, h), zero_mm));
              if (TMAP_VSW_UNLIKELY(cmp == 0xffff)) goto end_loop; // ACK: goto statement
          }
      }
end_loop:
      if(1 == query->type) { 
          // save the matrix
          for(j = 0; TMAP_VSW_LIKELY(j < slen); ++j) { // for each stripe in the query
              __tmap_vsw_mm_store_si128(H, __tmap_vsw_mm_load_si128(H1 + j));
              H++;
          }
      }
      /*
      fprintf(stderr, "i=%d", i);
      int32_t l;
      for(j = l = 0; j < tmap_vsw16_values_per_128_bits; j++) { // for each start position in the stripe
          for(k = 0; k < query->slen; k++, l++) {
              fprintf(stderr, " %d:%d", l, ((tmap_vsw16_int_t*)(H1+k))[j] - zero);
          }
      }
      fprintf(stderr, "\n");
      */
      //if(2 < i) tmap_error("full stop", Exit, OutOfRange);
      __tmap_vsw16_max(imax, max); // imax is the maximum number in max
      if(imax > gmax) { 
          gmax = imax; // global maximum score 
      }
      if(imax > best || (1 == direction && imax == best)) { // potential best score
          tmap_vsw16_int_t *t;
          if(query_end_clip == 0) { // check the last
              j = (query->qlen-1) % slen; // stripe
              k = (query->qlen-1) / slen; // byte
              t = (tmap_vsw16_int_t*)(H1 + j);
              //fprintf(stderr, "j=%d k=%d slen=%d qlen=%d best=%d pot=%d\n", j, k, slen, query->qlen, best-zero, t[k]-zero);
              if((int32_t)t[k] > best || (1 == direction && (int32_t)t[k] == best)) { // found
                  (*query_end) = query->qlen-1;
                  (*target_end) = i;
                  best = t[k];
                  //fprintf(stderr, "FOUND A i=%d query->qlen-1=%d query_end=%d target_end=%d best=%d\n", i, query->qlen-1, *query_end, *target_end, best-zero);
              }
          }
          else { // check all
              t = (tmap_vsw16_int_t*)H1;
              (*target_end) = i;
              for(j = 0, *query_end = -1; TMAP_VSW_LIKELY(j < slen); ++j) { // for each stripe in the query
                  for(k = 0; k < tmap_vsw16_values_per_128_bits; k++, t++) { // for each cell in the stripe
                      if((int32_t)*t > best || (1 == direction && (int32_t)*t== best)) { // found
                          best = *t;
                          *query_end = j + ((k & (tmap_vsw16_values_per_128_bits-1)) * slen);
                      }
                  }
              }
              if(-1 == *query_end) {
                  tmap_error("bug encountered", Exit, OutOfRange);
              }
              //fprintf(stderr, "FOUND B i=%d imax=%d best=%d query_end=%d target_end=%d\n", i, imax-zero, best-zero, *query_end, *target_end);
          }
      }
      if(query->max_aln_score - query->max_edit_score < imax) { // overflow
          if(NULL != overflow) {
              *overflow = 1;
              return tmap_vsw16_min_value;
          }
          // When overflow is going to happen, subtract tmap_vsw16_mid_value from all scores. This heuristic
          // may miss the best alignment, but in practice this should happen very rarely.
          sum += tmap_vsw16_mid_value; 
          if(query->min_aln_score + tmap_vsw16_mid_value < gmax) gmax -= tmap_vsw16_mid_value;
          else gmax = query->min_aln_score;
          for(j = 0; TMAP_VSW_LIKELY(j < slen); ++j) {
              h = __tmap_vsw16_mm_subs_epi16(__tmap_vsw_mm_load_si128(H1 + j), reduce_mm);
              __tmap_vsw_mm_store_si128(H1 + j, h);
              e = __tmap_vsw16_mm_subs_epi16(__tmap_vsw_mm_load_si128(E + j), reduce_mm);
              __tmap_vsw_mm_store_si128(E + j, e);
          }
      }
      // check for underflow
      __tmap_vsw16_min(imin, min); // imin is the minimum number in min
      if(imin < tmap_vsw16_min_value - query->min_edit_score) {
          tmap_error("bug encountered", Exit, OutOfRange);
      }
      S = H1; H1 = H0; H0 = S; // swap H0 and H1
  }
  if(tmap_vsw16_min_value == best) {
      (*query_end) = (*target_end) = -1;
      return best;
  }
  return best + sum - zero;
}

void
tmap_vsw16_sse2(tmap_vsw16_query_t *query_fwd, tmap_vsw16_query_t *query_rev, 
                uint8_t *target, int32_t tlen, 
                int32_t query_start_clip, int32_t query_end_clip,
                tmap_vsw_opt_t *opt, tmap_vsw_result_t *result, int32_t *overflow)
{
  // TODO: bounds check if we can fit into 16-byte values

  // forward
  result->score_fwd = tmap_vsw16_sse2_forward(query_fwd, target, tlen, 0, 0, query_start_clip, query_end_clip, 
                                              opt, &result->query_end, &result->target_end, 0, overflow);
  /*
  fprintf(stderr, "result = {%d-%d} {%d-%d} score=%d overflow=%d\n",
          result->query_start, result->query_end,
          result->target_start, result->target_end, 
          result->score_fwd, (*overflow));
          */
  if(NULL != overflow && 1 == *overflow) return;
  if(-1 == result->query_end) {
      tmap_error("bug encountered", Exit, OutOfRange);
  }

  // TODO: we could adjust the start position based on the forward
  // reverse
  tmap_reverse_compliment_int(target, tlen); // reverse compliment
  result->score_rev = tmap_vsw16_sse2_forward(query_rev, target, tlen, 
                                              query_rev->qlen - result->query_end - 1, tlen - result->target_end - 1,
                                              query_end_clip, query_start_clip, 
                                              opt, &result->query_start, &result->target_start, 1, overflow);
  if(-1 == result->query_start) {
      tmap_error("bug encountered", Exit, OutOfRange);
  }
  // adjust query/target start
  result->query_start = query_rev->qlen - result->query_start - 1; // zero-based
  result->target_start = tlen - result->target_start - 1; // zero-based
  tmap_reverse_compliment_int(target, tlen); // reverse compliment back
  // Debugging
  /*
  fprintf(stderr, "qlen=%d tlen=%d\n", query_fwd->qlen, tlen);
  fprintf(stderr, "result = {%d-%d} {%d-%d} score=%d\n",
          result->query_start, result->query_end,
          result->target_start, result->target_end, result->score_rev);
  fprintf(stderr, "result->score_fwd=%d result->score_rev=%d\n",
          result->score_fwd,
          result->score_rev);
          */
  /*
   * NB: I am not sure why these sometimes disagree, but they do.  Ignore for
   * now.
  */
  // check that they agree
  if(result->score_fwd != result->score_rev) {
      tmap_error("bug encountered", Exit, OutOfRange);
  }
}

void
tmap_vsw16_sse2_get_path(const uint8_t *query, int32_t qlen, 
                              const uint8_t *target, int32_t tlen, 
                              tmap_vsw16_query_t *q,
                              tmap_vsw_result_t *result, 
                              tmap_sw_path_t *path,
                              int32_t *path_len,
                              int32_t left_justify,
                              tmap_vsw_opt_t *opt)
{
  int32_t ti, qi;
  int32_t pen_gapoe, pen_gape;
  int32_t score, overflow = 0;
  uint32_t ctype;
  tmap_sw_path_t *p;
  tmap_vsw16_int_t h_cur;
  
  if(1 != q->type) { // ignore
      tmap_error("bug encountered", Exit, OutOfRange);
  }

  if(0 == qlen || 0 == tlen) {
      *path_len = 0;
      return; // smallest value
  }

  // store here
  pen_gapoe = opt->pen_gapo + opt->pen_gape; // gap open penalty
  pen_gape = opt->pen_gape; // gap extend penalty

  /*
  fprintf(stderr, "qlen=%d tlen=%d\n", qlen, tlen);
  fprintf(stderr, "result = {%d-%d} {%d-%d}\n",
          result->query_start, result->query_end,
          result->target_start, result->target_end);
  */
  
  // run the forward VSW
  score = tmap_vsw16_sse2_forward(q, target, tlen, 0, 0, 0, 0, opt, &qi, &ti, 0, &overflow);
  /*
  fprintf(stderr, "score=%d result->score_fwd=%d result->score_rev=%d\n",
          score,
          result->score_fwd,
          result->score_rev);
          */
  if(1 == overflow) {
      tmap_error("bug encountered", Exit, OutOfRange);
  }
  if(score != result->score_fwd) {
      tmap_error("bug encountered", Exit, OutOfRange);
  }
  if(tlen-1 != ti || qlen-1 != qi) {
      tmap_error("bug encountered", Exit, OutOfRange);
  }
  

  // trackback
  p = path;
  ti = tlen-1; qi = q->qlen-1;
  h_cur = __tmap_vsw16_get_matrix_cell(q, (ti < 0) ? 0 : ti, (qi < 0) ? 0 : qi) - q->zero_aln_score;
  ctype = TMAP_SW_FROM_M; 
  while(0 <= ti || 0 <= qi) {
      tmap_vsw16_int_t h_prev, e_prev, f_prev;
      // TODO: does not handle if there was overflow

      // get the cells to compare
      h_prev = __tmap_vsw16_get_matrix_cell(q, (ti-1 < 0) ? 0 : ti-1, (qi-1 < 0) ? 0 : qi-1) - q->zero_aln_score;
      e_prev = __tmap_vsw16_get_matrix_cell(q, (ti-1 < 0) ? 0 : ti-1, (qi < 0) ? 0 : qi) - q->zero_aln_score;
      f_prev = __tmap_vsw16_get_matrix_cell(q, (ti < 0) ? 0 : ti, (qi-1 < 0) ? 0 : qi-1) - q->zero_aln_score;

      // get the match score
      // TODO: should we be retrieving the score for every one ?
      score = __tmap_vsw16_get_query_profile_value(q, target[(ti < 0) ? 0 : ti], (qi < 0) ? 0 : qi);
      
      /*
      fprintf(stderr, "ti=%d qi=%d ctype=%d h_cur=%d h_prev=%d e_prev=%d f_prev=%d score=%d\n",
              ti, qi, ctype, 
              h_cur, h_prev, e_prev, f_prev, score);
      */

      // one-based
      p->i = ti+1; p->j = qi+1; 

      // corner cases
      if(0 == ti && 0 == qi) { // at the start
          h_cur = 0; 
          if(TMAP_SW_FROM_M == ctype) {
              if(-pen_gapoe < score) {
                  p->ctype = TMAP_SW_FROM_M;
              }
              else {
                  p->ctype = TMAP_SW_FROM_I;
              }
          }
          else if(TMAP_SW_FROM_I == ctype) {
              if(-pen_gape < score) {
                  p->ctype = TMAP_SW_FROM_M;
              }
              else {
                  p->ctype = TMAP_SW_FROM_I;
              }
          }
          else {
              tmap_error("bug encountered", Exit, OutOfRange);
          }
          p++;
          ti--;
          qi--;
          break;
      }
      else if(0 < ti && 0 == qi) { // leading deletion not allowed
          tmap_error("bug encountered", Exit, OutOfRange);
      }
      else if(0 == ti && 0 < qi) {
          // NB: this may not be correct
          while(0 == ti && 0 <= qi) {
              p->ctype = ctype;
              ctype = TMAP_SW_FROM_I; 
              p++;
              qi--;
          }
          ti--;
          break;
      }

      /*
       * Example:
       *
       * left-justify
       * AAAAAAAAA
       * SSSSIIIMM
       * SSSS---AA
       *
       * AAAA---AA
       * SSSMDDDMM
       * SSSAAAAAA
       *
       * right-justify
       * AAAAAAAAA
       * SSSSMMIII
       * SSSSAA---
       *
       * AAAAAA---
       * SSSMMMDDD
       * SSSAAAAAA
       */

      if(0 == left_justify) { // right-justify, choose indels first  
          if(ctype == TMAP_SW_FROM_M && h_cur == e_prev - pen_gapoe) {
              h_cur = e_prev;
              p->ctype = TMAP_SW_FROM_D; 
              ti--; 
          }
          else if(ctype == TMAP_SW_FROM_D && h_cur == e_prev - pen_gape) {
              h_cur = e_prev;
              p->ctype = TMAP_SW_FROM_D; 
              ti--; 
          }
          else if(ctype == TMAP_SW_FROM_M && h_cur == f_prev - pen_gapoe) {
              h_cur = f_prev;
              p->ctype = TMAP_SW_FROM_I; 
              qi--;
          }
          else if(ctype == TMAP_SW_FROM_I && h_cur == f_prev - pen_gape) {
              h_cur = f_prev;
              p->ctype = TMAP_SW_FROM_I; 
              qi--;
          }
          else if(h_cur == e_prev + score) {
              h_cur = e_prev;
              p->ctype = TMAP_SW_FROM_M; 
              ti--; qi--;
          }
          else if(h_cur == f_prev + score) {
              h_cur = f_prev;
              p->ctype = TMAP_SW_FROM_M; 
              ti--; qi--;
          }
          else if(h_cur == h_prev + score) {
              h_cur = h_prev;
              p->ctype = TMAP_SW_FROM_M; 
              ti--; qi--;
          }
          else {
              tmap_error("bug encountered", Exit, OutOfRange);
          }
      }
      else { // left-justify, choose matches first 
          // compare
          if(h_cur == h_prev + score) {
              h_cur = h_prev;
              p->ctype = TMAP_SW_FROM_M; 
              ti--; qi--;
          }
          else if(h_cur == e_prev + score) {
              h_cur = e_prev;
              p->ctype = TMAP_SW_FROM_M; 
              ti--; qi--;
          }
          else if(h_cur == f_prev + score) {
              h_cur = f_prev;
              p->ctype = TMAP_SW_FROM_M; 
              ti--; qi--;
          }
          else if(ctype == TMAP_SW_FROM_M && h_cur == e_prev - pen_gapoe) {
              h_cur = e_prev;
              p->ctype = TMAP_SW_FROM_D; 
              ti--; 
          }
          else if(ctype == TMAP_SW_FROM_D && h_cur == e_prev - pen_gape) {
              h_cur = e_prev;
              p->ctype = TMAP_SW_FROM_D; 
              ti--; 
          }
          else if(ctype == TMAP_SW_FROM_M && h_cur == f_prev - pen_gapoe) {
              h_cur = f_prev;
              p->ctype = TMAP_SW_FROM_I; 
              qi--;
          }
          else if(ctype == TMAP_SW_FROM_I && h_cur == f_prev - pen_gape) {
              h_cur = f_prev;
              p->ctype = TMAP_SW_FROM_I; 
              qi--;
          }
          else {
              tmap_error("bug encountered", Exit, OutOfRange);
          }
      }

      // move to the next path etc.
      ctype = p->ctype;
      p++;
  }
  *path_len = p - path;
}