/* Copyright (c) 2017-2018 Mozilla */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "kiss_fft.h"
#include "common.h"
#include <math.h>
#include "freq.h"
#include "pitch.h"
#include "arch.h"
#include "celt_lpc.h"
#include <assert.h>


#define PITCH_MIN_PERIOD 32
#define PITCH_MAX_PERIOD 256
#define PITCH_FRAME_SIZE 320
#define PITCH_BUF_SIZE (PITCH_MAX_PERIOD+PITCH_FRAME_SIZE)

#define CEPS_MEM 8
#define NB_DELTA_CEPS 6

#define NB_FEATURES (2*NB_BANDS+3+LPC_ORDER)

#define MULTI 4
#define MULTI_MASK (MULTI-1)


#include "ceps_codebooks.c"

#define SURVIVORS 5


void vq_quantize_mbest(const float *codebook, int nb_entries, const float *x, int ndim, int mbest, float *dist, int *index)
{
  int i, j;
  for (i=0;i<mbest;i++) dist[i] = 1e15;
  
  for (i=0;i<nb_entries;i++)
  {
    float d=0;
    for (j=0;j<ndim;j++)
      d += (x[j]-codebook[i*ndim+j])*(x[j]-codebook[i*ndim+j]);
    if (d<dist[mbest-1])
    {
      int pos;
      for (j=0;j<mbest-1;j++) {
        if (d < dist[j]) break;
      }
      pos = j;
      for (j=mbest-1;j>=pos+1;j--) {
        dist[j] = dist[j-1];
        index[j] = index[j-1];
      }
      dist[pos] = d;
      index[pos] = i;
    }
  }
}


int vq_quantize(const float *codebook, int nb_entries, const float *x, int ndim, float *dist)
{
  int i, j;
  float min_dist = 1e15;
  int nearest = 0;
  
  for (i=0;i<nb_entries;i++)
  {
    float dist=0;
    for (j=0;j<ndim;j++)
      dist += (x[j]-codebook[i*ndim+j])*(x[j]-codebook[i*ndim+j]);
    if (dist<min_dist)
    {
      min_dist = dist;
      nearest = i;
    }
  }
  if (dist)
    *dist = min_dist;
  return nearest;
}

#define NB_BANDS_1 (NB_BANDS - 1)
int quantize_2stage(float *x)
{
    int i;
    int id, id2, id3;
    float ref[NB_BANDS_1];
    RNN_COPY(ref, x, NB_BANDS_1);
    id = vq_quantize(ceps_codebook1, 1024, x, NB_BANDS_1, NULL);
    for (i=0;i<NB_BANDS_1;i++) {
        x[i] -= ceps_codebook1[id*NB_BANDS_1 + i];
    }
    id2 = vq_quantize(ceps_codebook2, 1024, x, NB_BANDS_1, NULL);
    for (i=0;i<NB_BANDS_1;i++) {
        x[i] -= ceps_codebook2[id2*NB_BANDS_1 + i];
    }
    id3 = vq_quantize(ceps_codebook3, 1024, x, NB_BANDS_1, NULL);
    for (i=0;i<NB_BANDS_1;i++) {
        x[i] = ceps_codebook1[id*NB_BANDS_1 + i] + ceps_codebook2[id2*NB_BANDS_1 + i] + ceps_codebook3[id3*NB_BANDS_1 + i];
    }
    if (0) {
        float err = 0;
        for (i=0;i<NB_BANDS_1;i++) {
            err += (x[i]-ref[i])*(x[i]-ref[i]);
        }
        printf("%f\n", sqrt(err/NB_BANDS));
    }
    
    return id;
}


int quantize_3stage_mbest(float *x, int entry[3])
{
    int i, k;
    int id, id2, id3;
    float ref[NB_BANDS_1];
    int curr_index[SURVIVORS];
    int index1[SURVIVORS][3];
    int index2[SURVIVORS][3];
    int index3[SURVIVORS][3];
    float curr_dist[SURVIVORS];
    float glob_dist[SURVIVORS];
    RNN_COPY(ref, x, NB_BANDS_1);
    vq_quantize_mbest(ceps_codebook1, 1024, x, NB_BANDS_1, SURVIVORS, curr_dist, curr_index);
    for (k=0;k<SURVIVORS;k++) {
      index1[k][0] = curr_index[k];
    }
    for (k=0;k<SURVIVORS;k++) {
      int m;
      float diff[NB_BANDS_1];
      for (i=0;i<NB_BANDS_1;i++) {
        diff[i] = x[i] - ceps_codebook1[index1[k][0]*NB_BANDS_1 + i];
      }
      vq_quantize_mbest(ceps_codebook2, 1024, diff, NB_BANDS_1, SURVIVORS, curr_dist, curr_index);
      if (k==0) {
        for (m=0;m<SURVIVORS;m++) {
          index2[m][0] = index1[k][0];
          index2[m][1] = curr_index[m];
          glob_dist[m] = curr_dist[m];
        }
        //printf("%f ", glob_dist[0]);
      } else if (curr_dist[0] < glob_dist[SURVIVORS-1]) {
        m=0;
        int pos;
        for (pos=0;pos<SURVIVORS;pos++) {
          if (curr_dist[m] < glob_dist[pos]) {
            int j;
            for (j=SURVIVORS-1;j>=pos+1;j--) {
              glob_dist[j] = glob_dist[j-1];
              index2[j][0] = index2[j-1][0];
              index2[j][1] = index2[j-1][1];
            }
            glob_dist[pos] = curr_dist[m];
            index2[pos][0] = index1[k][0];
            index2[pos][1] = curr_index[m];
            m++;
          }
        }
      }
    }
    for (k=0;k<SURVIVORS;k++) {
      int m;
      float diff[NB_BANDS_1];
      for (i=0;i<NB_BANDS_1;i++) {
        diff[i] = x[i] - ceps_codebook1[index2[k][0]*NB_BANDS_1 + i] - ceps_codebook2[index2[k][1]*NB_BANDS_1 + i];
      }
      vq_quantize_mbest(ceps_codebook3, 1024, diff, NB_BANDS_1, SURVIVORS, curr_dist, curr_index);
      if (k==0) {
        for (m=0;m<SURVIVORS;m++) {
          index3[m][0] = index2[k][0];
          index3[m][1] = index2[k][1];
          index3[m][2] = curr_index[m];
          glob_dist[m] = curr_dist[m];
        }
        //printf("%f ", glob_dist[0]);
      } else if (curr_dist[0] < glob_dist[SURVIVORS-1]) {
        m=0;
        int pos;
        for (pos=0;pos<SURVIVORS;pos++) {
          if (curr_dist[m] < glob_dist[pos]) {
            int j;
            for (j=SURVIVORS-1;j>=pos+1;j--) {
              glob_dist[j] = glob_dist[j-1];
              index3[j][0] = index3[j-1][0];
              index3[j][1] = index3[j-1][1];
              index3[j][2] = index3[j-1][2];
            }
            glob_dist[pos] = curr_dist[m];
            index3[pos][0] = index2[k][0];
            index3[pos][1] = index2[k][1];
            index3[pos][2] = curr_index[m];
            m++;
          }
        }
      }
    }
    entry[0] = id = index3[0][0];
    entry[1] = id2 = index3[0][1];
    entry[2] = id3 = index3[0][2];
    //printf("%f ", glob_dist[0]);
    for (i=0;i<NB_BANDS_1;i++) {
        x[i] -= ceps_codebook1[id*NB_BANDS_1 + i];
    }
    for (i=0;i<NB_BANDS_1;i++) {
        x[i] -= ceps_codebook2[id2*NB_BANDS_1 + i];
    }
    //id3 = vq_quantize(ceps_codebook3, 1024, x, NB_BANDS_1, NULL);
    for (i=0;i<NB_BANDS_1;i++) {
        x[i] = ceps_codebook1[id*NB_BANDS_1 + i] + ceps_codebook2[id2*NB_BANDS_1 + i] + ceps_codebook3[id3*NB_BANDS_1 + i];
    }
    if (0) {
        float err = 0;
        for (i=0;i<NB_BANDS_1;i++) {
            err += (x[i]-ref[i])*(x[i]-ref[i]);
        }
        printf("%f\n", sqrt(err/NB_BANDS));
    }
    
    return id;
}

static int find_nearest_multi(const float *codebook, int nb_entries, const float *x, int ndim, float *dist, int sign)
{
  int i, j;
  float min_dist = 1e15;
  int nearest = 0;

  for (i=0;i<nb_entries;i++)
  {
    int offset;
    float dist=0;
    offset = (i&MULTI_MASK)*ndim;
    for (j=0;j<ndim;j++)
      dist += (x[offset+j]-codebook[i*ndim+j])*(x[offset+j]-codebook[i*ndim+j]);
    if (dist<min_dist)
    {
      min_dist = dist;
      nearest = i;
    }
  }
  if (sign) {
    for (i=0;i<nb_entries;i++)
    {
      int offset;
      float dist=0;
      offset = (i&MULTI_MASK)*ndim;
      for (j=0;j<ndim;j++)
        dist += (x[offset+j]+codebook[i*ndim+j])*(x[offset+j]+codebook[i*ndim+j]);
      if (dist<min_dist)
      {
        min_dist = dist;
        nearest = i+nb_entries;
      }
    }
  }
  if (dist)
    *dist = min_dist;
  return nearest;
}


int quantize_diff(float *x, float *left, float *right, float *codebook, int bits, int sign, int *entry)
{
    int i;
    int nb_entries;
    int id;
    float ref[NB_BANDS];
    float pred[4*NB_BANDS];
    float target[4*NB_BANDS];
    float s = 1;
    nb_entries = 1<<bits;
    RNN_COPY(ref, x, NB_BANDS);
    for (i=0;i<NB_BANDS;i++) pred[i] = pred[NB_BANDS+i] = .5*(left[i] + right[i]);
    for (i=0;i<NB_BANDS;i++) pred[2*NB_BANDS+i] = left[i];
    for (i=0;i<NB_BANDS;i++) pred[3*NB_BANDS+i] = right[i];
    for (i=0;i<4*NB_BANDS;i++) target[i] = x[i%NB_BANDS] - pred[i];

    id = find_nearest_multi(codebook, nb_entries, target, NB_BANDS, NULL, sign);
    *entry = id;
    if (id >= 1<<bits) {
      s = -1;
      id -= (1<<bits);
    }
    for (i=0;i<NB_BANDS;i++) {
      x[i] = pred[(id&MULTI_MASK)*NB_BANDS + i] + s*codebook[id*NB_BANDS + i];
    }
    //printf("%d %f ", id&MULTI_MASK, s);
    if (0) {
        float err = 0;
        for (i=0;i<NB_BANDS;i++) {
            err += (x[i]-ref[i])*(x[i]-ref[i]);
        }
        printf("%f\n", sqrt(err/NB_BANDS));
    }
    
    return id;
}

#define FORBIDDEN_INTERP 7

int interp_search(const float *x, const float *left, const float *right, float *dist_out)
{
    int i, k;
    float min_dist = 1e15;
    int best_pred = 0;
    float pred[4*NB_BANDS];
    for (i=0;i<NB_BANDS;i++) pred[i] = pred[NB_BANDS+i] = .5*(left[i] + right[i]);
    for (i=0;i<NB_BANDS;i++) pred[2*NB_BANDS+i] = left[i];
    for (i=0;i<NB_BANDS;i++) pred[3*NB_BANDS+i] = right[i];

    for (k=1;k<4;k++) {
      float dist = 0;
      for (i=0;i<NB_BANDS;i++) dist += (x[i] - pred[k*NB_BANDS+i])*(x[i] - pred[k*NB_BANDS+i]);
      dist_out[k-1] = dist;
      if (dist < min_dist) {
        min_dist = dist;
        best_pred = k;
      }
    }
    return best_pred - 1;
}


void interp_diff(float *x, float *left, float *right, float *codebook, int bits, int sign)
{
    int i, k;
    float min_dist = 1e15;
    int best_pred = 0;
    float ref[NB_BANDS];
    float pred[4*NB_BANDS];
    (void)sign;
    (void)codebook;
    (void)bits;
    RNN_COPY(ref, x, NB_BANDS);
    for (i=0;i<NB_BANDS;i++) pred[i] = pred[NB_BANDS+i] = .5*(left[i] + right[i]);
    for (i=0;i<NB_BANDS;i++) pred[2*NB_BANDS+i] = left[i];
    for (i=0;i<NB_BANDS;i++) pred[3*NB_BANDS+i] = right[i];

    for (k=1;k<4;k++) {
      float dist = 0;
      for (i=0;i<NB_BANDS;i++) dist += (x[i] - pred[k*NB_BANDS+i])*(x[i] - pred[k*NB_BANDS+i]);
      if (dist < min_dist) {
        min_dist = dist;
        best_pred = k;
      }
    }
    //printf("%d ", best_pred);
    for (i=0;i<NB_BANDS;i++) {
      x[i] = pred[best_pred*NB_BANDS + i];
    }
    if (0) {
        float err = 0;
        for (i=0;i<NB_BANDS;i++) {
            err += (x[i]-ref[i])*(x[i]-ref[i]);
        }
        printf("%f\n", sqrt(err/NB_BANDS));
    }
}

int double_interp_search(const float features[4][NB_FEATURES], const float *mem) {
    int i, j;
    int best_id=0;
    float min_dist = 1e15;
    float dist[2][3];
    interp_search(features[0], mem, features[1], dist[0]);
    interp_search(features[2], features[1], features[3], dist[1]);
    for (i=0;i<3;i++) {
        for (j=0;j<3;j++) {
            float d;
            int id;
            id = 3*i + j;
            d = dist[0][i] + dist[1][j];
            if (d < min_dist && id != FORBIDDEN_INTERP) {
                min_dist = d;
                best_id = id;
            }
        }
    }
    //printf("%d %d %f    %d %f\n", id0, id1, dist[0][id0] + dist[1][id1], best_id, min_dist);
    return best_id - (best_id >= FORBIDDEN_INTERP);
}

static void single_interp(float *x, const float *left, const float *right, int id)
{
    int i;
    float ref[NB_BANDS];
    float pred[3*NB_BANDS];
    RNN_COPY(ref, x, NB_BANDS);
    for (i=0;i<NB_BANDS;i++) pred[i] = .5*(left[i] + right[i]);
    for (i=0;i<NB_BANDS;i++) pred[NB_BANDS+i] = left[i];
    for (i=0;i<NB_BANDS;i++) pred[2*NB_BANDS+i] = right[i];
    for (i=0;i<NB_BANDS;i++) {
      x[i] = pred[id*NB_BANDS + i];
    }
    if (0) {
        float err = 0;
        for (i=0;i<NB_BANDS;i++) {
            err += (x[i]-ref[i])*(x[i]-ref[i]);
        }
        printf("%f\n", sqrt(err/NB_BANDS));
    }
}

void perform_double_interp(float features[4][NB_FEATURES], const float *mem, int best_id) {
    int id0, id1;
    best_id += (best_id >= FORBIDDEN_INTERP);
    id0 = best_id / 3;
    id1 = best_id % 3;
    single_interp(features[0], mem, features[1], id0);
    single_interp(features[2], features[1], features[3], id1);
}

void perform_interp_relaxation(float features[4][NB_FEATURES], const float *mem) {
    int id0, id1;
    int best_id;
    int i;
    float count, count_1;
    best_id = double_interp_search(features, mem);
    best_id += (best_id >= FORBIDDEN_INTERP);
    id0 = best_id / 3;
    id1 = best_id % 3;
    count = 1;
    if (id0 != 1) {
        float t = (id0==0) ? .5 : 1.;
        for (i=0;i<NB_BANDS;i++) features[1][i] += t*features[0][i];
        count += t;
    }
    if (id1 != 2) {
        float t = (id1==0) ? .5 : 1.;
        for (i=0;i<NB_BANDS;i++) features[1][i] += t*features[2][i];
        count += t;
    }
    count_1 = 1.f/count;
    for (i=0;i<NB_BANDS;i++) features[1][i] *= count_1;
}

#define BITS_PER_CHAR 8
typedef struct {
    int byte_pos;
    int bit_pos;
    int max_bytes;
    unsigned char *chars;
} packer;

typedef struct {
    int byte_pos;
    int bit_pos;
    int max_bytes;
    const unsigned char *chars;
} unpacker;

void bits_packer_init(packer *bits, unsigned char *buf, int size) {
  bits->byte_pos = 0;
  bits->bit_pos = 0;
  bits->max_bytes = size;
  bits->chars = buf;
  RNN_CLEAR(buf, size);
}

void bits_unpacker_init(unpacker *bits, unsigned char *buf, int size) {
  bits->byte_pos = 0;
  bits->bit_pos = 0;
  bits->max_bytes = size;
  bits->chars = buf;
}

void bits_pack(packer *bits, unsigned int data, int nb_bits) {
  while(nb_bits)
  {
    int bit;
    if (bits->byte_pos == bits->max_bytes) {
      fprintf(stderr, "something went horribly wrong\n");
      return;
    }
    bit = (data>>(nb_bits-1))&1;
    bits->chars[bits->byte_pos] |= bit<<(BITS_PER_CHAR-1-bits->bit_pos);
    bits->bit_pos++;

    if (bits->bit_pos==BITS_PER_CHAR)
    {
      bits->bit_pos=0;
      bits->byte_pos++;
      if (bits->byte_pos < bits->max_bytes) bits->chars[bits->byte_pos] = 0;
    }
    nb_bits--;
  }
}

unsigned int bits_unpack(unpacker *bits, int nb_bits) {
  unsigned int d=0;
  while(nb_bits)
  {
    if (bits->byte_pos == bits->max_bytes) {
      fprintf(stderr, "something went horribly wrong\n");
      return 0;
    }
    d<<=1;
    d |= (bits->chars[bits->byte_pos]>>(BITS_PER_CHAR-1 - bits->bit_pos))&1;
    bits->bit_pos++;
    if (bits->bit_pos==BITS_PER_CHAR)
    {
      bits->bit_pos=0;
      bits->byte_pos++;
    }
    nb_bits--;
  }
  return d;
}

typedef struct {
  float analysis_mem[OVERLAP_SIZE];
  float cepstral_mem[CEPS_MEM][NB_BANDS];
  int pcount;
  float pitch_mem[LPC_ORDER];
  float pitch_filt;
  float xc[10][PITCH_MAX_PERIOD+1];
  float frame_weight[10];
  float exc_buf[PITCH_BUF_SIZE];
  float pitch_max_path[2][PITCH_MAX_PERIOD];
  float pitch_max_path_all;
  int best_i;
  float last_gain;
  int last_period;
  float lpc[LPC_ORDER];
  float vq_mem[NB_BANDS];
  float features[4][NB_FEATURES];
  float sig_mem[LPC_ORDER];
  int exc_mem;
} DenoiseState;

static int rnnoise_get_size() {
  return sizeof(DenoiseState);
}

static int rnnoise_init(DenoiseState *st) {
  memset(st, 0, sizeof(*st));
  return 0;
}

static DenoiseState *rnnoise_create() {
  DenoiseState *st;
  st = malloc(rnnoise_get_size());
  rnnoise_init(st);
  return st;
}

static void rnnoise_destroy(DenoiseState *st) {
  free(st);
}

static short float2short(float x)
{
  int i;
  i = (int)floor(.5+x);
  return IMAX(-32767, IMIN(32767, i));
}

static void frame_analysis(DenoiseState *st, kiss_fft_cpx *X, float *Ex, const float *in) {
  float x[WINDOW_SIZE];
  RNN_COPY(x, st->analysis_mem, OVERLAP_SIZE);
  RNN_COPY(&x[OVERLAP_SIZE], in, FRAME_SIZE);
  RNN_COPY(st->analysis_mem, &in[FRAME_SIZE-OVERLAP_SIZE], OVERLAP_SIZE);
  apply_window(x);
  forward_transform(X, x);
  compute_band_energy(Ex, X);
}

static void compute_frame_features(DenoiseState *st, const float *in) {
  float aligned_in[FRAME_SIZE];
  int i;
  float E = 0;
  float Ly[NB_BANDS];
  float follow, logMax;
  float g;
  kiss_fft_cpx X[FREQ_SIZE];
  float Ex[NB_BANDS];
  float xcorr[PITCH_MAX_PERIOD];
  float ener0;
  int sub;
  float ener;
  RNN_COPY(aligned_in, &st->analysis_mem[OVERLAP_SIZE-TRAINING_OFFSET], TRAINING_OFFSET);
  frame_analysis(st, X, Ex, in);
  logMax = -2;
  follow = -2;
  for (i=0;i<NB_BANDS;i++) {
    Ly[i] = log10(1e-2+Ex[i]);
    Ly[i] = MAX16(logMax-8, MAX16(follow-2.5, Ly[i]));
    logMax = MAX16(logMax, Ly[i]);
    follow = MAX16(follow-2.5, Ly[i]);
    E += Ex[i];
  }
  dct(st->features[st->pcount], Ly);
  st->features[st->pcount][0] -= 4;
  g = lpc_from_cepstrum(st->lpc, st->features[st->pcount]);
  st->features[st->pcount][2*NB_BANDS+2] = log10(g);
  for (i=0;i<LPC_ORDER;i++) st->features[st->pcount][2*NB_BANDS+3+i] = st->lpc[i];
  RNN_MOVE(st->exc_buf, &st->exc_buf[FRAME_SIZE], PITCH_MAX_PERIOD);
  RNN_COPY(&aligned_in[TRAINING_OFFSET], in, FRAME_SIZE-TRAINING_OFFSET);
  for (i=0;i<FRAME_SIZE;i++) {
    int j;
    float sum = aligned_in[i];
    for (j=0;j<LPC_ORDER;j++)
      sum += st->lpc[j]*st->pitch_mem[j];
    RNN_MOVE(st->pitch_mem+1, st->pitch_mem, LPC_ORDER-1);
    st->pitch_mem[0] = aligned_in[i];
    st->exc_buf[PITCH_MAX_PERIOD+i] = sum + .7*st->pitch_filt;
    st->pitch_filt = sum;
    //printf("%f\n", st->exc_buf[PITCH_MAX_PERIOD+i]);
  }
  /* Cross-correlation on half-frames. */
  for (sub=0;sub<2;sub++) {
    int off = sub*FRAME_SIZE/2;
    celt_pitch_xcorr(&st->exc_buf[PITCH_MAX_PERIOD+off], st->exc_buf+off, xcorr, FRAME_SIZE/2, PITCH_MAX_PERIOD);
    ener0 = celt_inner_prod(&st->exc_buf[PITCH_MAX_PERIOD+off], &st->exc_buf[PITCH_MAX_PERIOD+off], FRAME_SIZE/2);
    st->frame_weight[2+2*st->pcount+sub] = ener0;
    //printf("%f\n", st->frame_weight[2+2*st->pcount+sub]);
    for (i=0;i<PITCH_MAX_PERIOD;i++) {
      ener = (1 + ener0 + celt_inner_prod(&st->exc_buf[i+off], &st->exc_buf[i+off], FRAME_SIZE/2));
      st->xc[2+2*st->pcount+sub][i] = 2*xcorr[i] / ener;
    }
#if 0
    for (i=0;i<PITCH_MAX_PERIOD;i++)
      printf("%f ", st->xc[2*st->pcount+sub][i]);
    printf("\n");
#endif
  }
}

static void process_superframe(DenoiseState *st, FILE *ffeat, int encode, int quantize) {
  int i;
  int sub;
  int best_i;
  int best[10];
  int pitch_prev[8][PITCH_MAX_PERIOD];
  float best_a=0;
  float best_b=0;
  float w;
  float sx=0, sxx=0, sxy=0, sy=0, sw=0;
  float frame_corr;
  int voiced;
  float frame_weight_sum = 1e-15;
  float center_pitch;
  int main_pitch;
  int modulation;
  int c0_id=0;
  int vq_end[3]={0};
  int vq_mid=0;
  int corr_id = 0;
  int interp_id=0;
  for(sub=0;sub<8;sub++) frame_weight_sum += st->frame_weight[2+sub];
  for(sub=0;sub<8;sub++) st->frame_weight[2+sub] *= (8.f/frame_weight_sum);
  for(sub=0;sub<8;sub++) {
    float max_path_all = -1e15;
    best_i = 0;
    for (i=0;i<PITCH_MAX_PERIOD-2*PITCH_MIN_PERIOD;i++) {
      float xc_half = MAX16(MAX16(st->xc[2+sub][(PITCH_MAX_PERIOD+i)/2], st->xc[2+sub][(PITCH_MAX_PERIOD+i+2)/2]), st->xc[2+sub][(PITCH_MAX_PERIOD+i-1)/2]);
      if (st->xc[2+sub][i] < xc_half*1.1) st->xc[2+sub][i] *= .8;
    }
    for (i=0;i<PITCH_MAX_PERIOD-PITCH_MIN_PERIOD;i++) {
      int j;
      float max_prev;
      max_prev = st->pitch_max_path_all - 6.f;
      pitch_prev[sub][i] = st->best_i;
      for (j=IMIN(0, 4-i);j<=4 && i+j<PITCH_MAX_PERIOD-PITCH_MIN_PERIOD;j++) {
        if (st->pitch_max_path[0][i+j] > max_prev) {
          max_prev = st->pitch_max_path[0][i+j] - .02f*abs(j)*abs(j);
          pitch_prev[sub][i] = i+j;
        }
      }
      st->pitch_max_path[1][i] = max_prev + st->frame_weight[2+sub]*st->xc[2+sub][i];
      if (st->pitch_max_path[1][i] > max_path_all) {
        max_path_all = st->pitch_max_path[1][i];
        best_i = i;
      }
    }
    /* Renormalize. */
    for (i=0;i<PITCH_MAX_PERIOD-PITCH_MIN_PERIOD;i++) st->pitch_max_path[1][i] -= max_path_all;
    //for (i=0;i<PITCH_MAX_PERIOD-PITCH_MIN_PERIOD;i++) printf("%f ", st->pitch_max_path[1][i]);
    //printf("\n");
    RNN_COPY(&st->pitch_max_path[0][0], &st->pitch_max_path[1][0], PITCH_MAX_PERIOD);
    st->pitch_max_path_all = max_path_all;
    st->best_i = best_i;
  }
  best_i = st->best_i;
  frame_corr = 0;
  /* Backward pass. */
  for (sub=7;sub>=0;sub--) {
    best[2+sub] = PITCH_MAX_PERIOD-best_i;
    frame_corr += st->frame_weight[2+sub]*st->xc[2+sub][best_i];
    best_i = pitch_prev[sub][best_i];
  }
  frame_corr /= 8;
  if (quantize && frame_corr < 0) frame_corr = 0;
  for (sub=0;sub<8;sub++) {
    //printf("%d %f\n", best[2+sub], frame_corr);
  }
  //printf("\n");
  for (sub=2;sub<10;sub++) {
    w = st->frame_weight[sub];
    sw += w;
    sx += w*sub;
    sxx += w*sub*sub;
    sxy += w*sub*best[sub];
    sy += w*best[sub];
  }
  voiced = frame_corr >= .3;
  /* Linear regression to figure out the pitch contour. */
  best_a = (sw*sxy - sx*sy)/(sw*sxx - sx*sx);
  if (voiced) {
    float max_a;
    float mean_pitch = sy/sw;
    /* Allow a relative variation of up to 1/4 over 8 sub-frames. */
    max_a = mean_pitch/32;
    best_a = MIN16(max_a, MAX16(-max_a, best_a));
    corr_id = (int)floor((frame_corr-.3f)/.175f);
    if (quantize) frame_corr = 0.3875f + .175f*corr_id;
  } else {
    best_a = 0;
    corr_id = (int)floor(frame_corr/.075f);
    if (quantize) frame_corr = 0.0375f + .075f*corr_id;
  }
  //best_b = (sxx*sy - sx*sxy)/(sw*sxx - sx*sx);
  best_b = (sy - best_a*sx)/sw;
  /* Quantizing the pitch as "main" pitch + slope. */
  center_pitch = best_b+5.5*best_a;
  main_pitch = (int)floor(.5 + 21.*log2(center_pitch/PITCH_MIN_PERIOD));
  main_pitch = IMAX(0, IMIN(63, main_pitch));
  modulation = (int)floor(.5 + 16*7*best_a/center_pitch);
  modulation = IMAX(-3, IMIN(3, modulation));
  //printf("%d %d\n", main_pitch, modulation);
  //printf("%f %f\n", best_a/center_pitch, best_corr);
  //for (sub=2;sub<10;sub++) printf("%f %d %f\n", best_b + sub*best_a, best[sub], best_corr);
  for (sub=0;sub<4;sub++) {
    if (quantize) {
      float p = pow(2.f, main_pitch/21.)*PITCH_MIN_PERIOD;
      p *= 1 + modulation/16./7.*(2*sub-3);
      st->features[sub][2*NB_BANDS] = .02*(p-100);
      st->features[sub][2*NB_BANDS + 1] = frame_corr-.5;
    } else {
      st->features[sub][2*NB_BANDS] = .01*(best[2+2*sub]+best[2+2*sub+1]-200);
      st->features[sub][2*NB_BANDS + 1] = frame_corr-.5;
    }
    //printf("%f %d %f\n", st->features[sub][2*NB_BANDS], best[2+2*sub], frame_corr);
  }
  //printf("%d %f %f %f\n", best_period, best_a, best_b, best_corr);
  RNN_COPY(&st->xc[0][0], &st->xc[8][0], PITCH_MAX_PERIOD);
  RNN_COPY(&st->xc[1][0], &st->xc[9][0], PITCH_MAX_PERIOD);
  if (quantize) {
    //printf("%f\n", st->features[3][0]);
    c0_id = (int)floor(.5 + st->features[3][0]*4);
    c0_id = IMAX(-64, IMIN(63, c0_id));
    st->features[3][0] = c0_id/4.;
    quantize_3stage_mbest(&st->features[3][1], vq_end);
    /*perform_interp_relaxation(st->features, st->vq_mem);*/
    quantize_diff(&st->features[1][0], st->vq_mem, &st->features[3][0], ceps_codebook_diff4, 12, 1, &vq_mid);
    interp_id = double_interp_search(st->features, st->vq_mem);
    perform_double_interp(st->features, st->vq_mem, interp_id);
  }
  for (sub=0;sub<4;sub++) {
    float g = lpc_from_cepstrum(st->lpc, st->features[sub]);
    st->features[sub][2*NB_BANDS+2] = log10(g);
    for (i=0;i<LPC_ORDER;i++) st->features[sub][2*NB_BANDS+3+i] = st->lpc[i];
  }
  //printf("\n");
  RNN_COPY(st->vq_mem, &st->features[3][0], NB_BANDS);
  if (encode) {
    unsigned char buf[8];
    packer bits;
    //fprintf(stdout, "%d %d %d %d %d %d %d %d %d\n", c0_id+64, main_pitch, voiced ? modulation+4 : 0, corr_id, vq_end[0], vq_end[1], vq_end[2], vq_mid, interp_id);
    bits_packer_init(&bits, buf, 8);
    bits_pack(&bits, c0_id+64, 7);
    bits_pack(&bits, main_pitch, 6);
    bits_pack(&bits, voiced ? modulation+4 : 0, 3);
    bits_pack(&bits, corr_id, 2);
    bits_pack(&bits, vq_end[0], 10);
    bits_pack(&bits, vq_end[1], 10);
    bits_pack(&bits, vq_end[2], 10);
    bits_pack(&bits, vq_mid, 13);
    bits_pack(&bits, interp_id, 3);
    fwrite(buf, 1, 8, ffeat);
  } else {
    for (i=0;i<4;i++) {
      fwrite(st->features[i], sizeof(float), NB_FEATURES, ffeat);
    }
  }
}

void decode_packet(FILE *ffeat, float *vq_mem, unsigned char buf[8])
{
  int c0_id;
  int main_pitch;
  int modulation;
  int corr_id;
  int vq_end[3];
  int vq_mid;
  int interp_id;
  
  int i;
  int sub;
  int voiced = 1;
  float frame_corr;
  float features[4][NB_FEATURES];
  unpacker bits;
  
  bits_unpacker_init(&bits, buf, 8);
  c0_id = bits_unpack(&bits, 7);
  main_pitch = bits_unpack(&bits, 6);
  modulation = bits_unpack(&bits, 3);
  corr_id = bits_unpack(&bits, 2);
  vq_end[0] = bits_unpack(&bits, 10);
  vq_end[1] = bits_unpack(&bits, 10);
  vq_end[2] = bits_unpack(&bits, 10);
  vq_mid = bits_unpack(&bits, 13);
  interp_id = bits_unpack(&bits, 3);
  //fprintf(stdout, "%d %d %d %d %d %d %d %d %d\n", c0_id, main_pitch, modulation, corr_id, vq_end[0], vq_end[1], vq_end[2], vq_mid, interp_id);

  
  for (i=0;i<4;i++) RNN_CLEAR(&features[i][0], NB_FEATURES);

  modulation -= 4;
  if (modulation==-4) {
    voiced = 0;
    modulation = 0;
  }
  if (voiced) {
    frame_corr = 0.3875f + .175f*corr_id;
  } else {
    frame_corr = 0.0375f + .075f*corr_id;
  }
  for (sub=0;sub<4;sub++) {
    float p = pow(2.f, main_pitch/21.)*PITCH_MIN_PERIOD;
    p *= 1 + modulation/16./7.*(2*sub-3);
    features[sub][2*NB_BANDS] = .02*(p-100);
    features[sub][2*NB_BANDS + 1] = frame_corr-.5;
  }
  
  features[3][0] = (c0_id-64)/4.;
  for (i=0;i<NB_BANDS_1;i++) {
    features[3][i+1] = ceps_codebook1[vq_end[0]*NB_BANDS_1 + i] + ceps_codebook2[vq_end[1]*NB_BANDS_1 + i] + ceps_codebook3[vq_end[2]*NB_BANDS_1 + i];
  }

  float sign = 1;
  if (vq_mid >= 4096) {
    vq_mid -= 4096;
    sign = -1;
  }
  for (i=0;i<NB_BANDS;i++) {
    features[1][i] = sign*ceps_codebook_diff4[vq_mid*NB_BANDS + i];
  }
  if ((vq_mid&MULTI_MASK) < 2) {
    for (i=0;i<NB_BANDS;i++) features[1][i] += .5*(vq_mem[i] + features[3][i]);
  } else if ((vq_mid&MULTI_MASK) == 2) {
    for (i=0;i<NB_BANDS;i++) features[1][i] += vq_mem[i];
  } else {
    for (i=0;i<NB_BANDS;i++) features[1][i] += features[3][i];
  }
  
  perform_double_interp(features, vq_mem, interp_id);

  RNN_COPY(vq_mem, &features[3][0], NB_BANDS);
  for (i=0;i<4;i++) {
    fwrite(features[i], sizeof(float), NB_FEATURES, ffeat);
  }
}

static void biquad(float *y, float mem[2], const float *x, const float *b, const float *a, int N) {
  int i;
  for (i=0;i<N;i++) {
    float xi, yi;
    xi = x[i];
    yi = x[i] + mem[0];
    mem[0] = mem[1] + (b[0]*(double)xi - a[0]*(double)yi);
    mem[1] = (b[1]*(double)xi - a[1]*(double)yi);
    y[i] = yi;
  }
}

static void preemphasis(float *y, float *mem, const float *x, float coef, int N) {
  int i;
  for (i=0;i<N;i++) {
    float yi;
    yi = x[i] + *mem;
    *mem = -coef*x[i];
    y[i] = yi;
  }
}

static float uni_rand() {
  return rand()/(double)RAND_MAX-.5;
}

static void rand_resp(float *a, float *b) {
  a[0] = .75*uni_rand();
  a[1] = .75*uni_rand();
  b[0] = .75*uni_rand();
  b[1] = .75*uni_rand();
}

void compute_noise(int *noise, float noise_std) {
  int i;
  for (i=0;i<FRAME_SIZE;i++) {
    noise[i] = (int)floor(.5 + noise_std*.707*(log_approx((float)rand()/RAND_MAX)-log_approx((float)rand()/RAND_MAX)));
  }
}


void write_audio(DenoiseState *st, const short *pcm, const int *noise, FILE *file) {
  int i, k;
  for (k=0;k<4;k++) {
  unsigned char data[4*FRAME_SIZE];
  for (i=0;i<FRAME_SIZE;i++) {
    float p=0;
    float e;
    int j;
    for (j=0;j<LPC_ORDER;j++) p -= st->features[k][2*NB_BANDS+3+j]*st->sig_mem[j];
    e = lin2ulaw(pcm[k*FRAME_SIZE+i] - p);
    /* Signal. */
    data[4*i] = lin2ulaw(st->sig_mem[0]);
    /* Prediction. */
    data[4*i+1] = lin2ulaw(p);
    /* Excitation in. */
    data[4*i+2] = st->exc_mem;
    /* Excitation out. */
    data[4*i+3] = e;
    /* Simulate error on excitation. */
    e += noise[k*FRAME_SIZE+i];
    e = IMIN(255, IMAX(0, e));
    
    RNN_MOVE(&st->sig_mem[1], &st->sig_mem[0], LPC_ORDER-1);
    st->sig_mem[0] = p + ulaw2lin(e);
    st->exc_mem = e;
  }
  fwrite(data, 4*FRAME_SIZE, 1, file);
  }
}

int main(int argc, char **argv) {
  int i;
  int count=0;
  static const float a_hp[2] = {-1.99599, 0.99600};
  static const float b_hp[2] = {-2, 1};
  float a_sig[2] = {0};
  float b_sig[2] = {0};
  float mem_hp_x[2]={0};
  float mem_resp_x[2]={0};
  float mem_preemph=0;
  float x[FRAME_SIZE];
  int gain_change_count=0;
  FILE *f1;
  FILE *ffeat;
  FILE *fpcm=NULL;
  short pcm[FRAME_SIZE]={0};
  short pcmbuf[FRAME_SIZE*4]={0};
  int noisebuf[FRAME_SIZE*4]={0};
  short tmp[FRAME_SIZE] = {0};
  float savedX[FRAME_SIZE] = {0};
  float speech_gain=1;
  int last_silent = 1;
  float old_speech_gain = 1;
  int one_pass_completed = 0;
  DenoiseState *st;
  float noise_std=0;
  int training = -1;
  int encode = 0;
  int decode = 0;
  int quantize = 0;
  st = rnnoise_create();
  if (argc == 5 && strcmp(argv[1], "-train")==0) training = 1;
  if (argc == 5 && strcmp(argv[1], "-qtrain")==0) {
      training = 1;
      quantize = 1;
  }
  if (argc == 4 && strcmp(argv[1], "-test")==0) training = 0;
  if (argc == 4 && strcmp(argv[1], "-qtest")==0) {
      training = 0;
      quantize = 1;
  }
  if (argc == 4 && strcmp(argv[1], "-encode")==0) {
      training = 0;
      quantize = 1;
      encode = 1;
  }
  if (argc == 4 && strcmp(argv[1], "-decode")==0) {
      training = 0;
      decode = 1;
  }
  if (training == -1) {
    fprintf(stderr, "usage: %s -train <speech> <features out> <pcm out>\n", argv[0]);
    fprintf(stderr, "  or   %s -test <speech> <features out>\n", argv[0]);
    return 1;
  }
  f1 = fopen(argv[2], "r");
  if (f1 == NULL) {
    fprintf(stderr,"Error opening input .s16 16kHz speech input file: %s\n", argv[2]);
    exit(1);
  }
  ffeat = fopen(argv[3], "w");
  if (ffeat == NULL) {
    fprintf(stderr,"Error opening output feature file: %s\n", argv[3]);
    exit(1);
  }
  if (decode) {
    float vq_mem[NB_BANDS] = {0};
    while (1) {
      int ret;
      unsigned char buf[8];
      //int c0_id, main_pitch, modulation, corr_id, vq_end[3], vq_mid, interp_id;
      //ret = fscanf(f1, "%d %d %d %d %d %d %d %d %d\n", &c0_id, &main_pitch, &modulation, &corr_id, &vq_end[0], &vq_end[1], &vq_end[2], &vq_mid, &interp_id);
      ret = fread(buf, 1, 8, f1);
      if (ret != 8) break;
      decode_packet(ffeat, vq_mem, buf);
    }
    return 0;
  }
  if (training) {
    fpcm = fopen(argv[4], "w");
    if (fpcm == NULL) {
      fprintf(stderr,"Error opening output PCM file: %s\n", argv[4]);
      exit(1);
    }
  }
  while (1) {
    float E=0;
    int silent;
    for (i=0;i<FRAME_SIZE;i++) x[i] = tmp[i];
    fread(tmp, sizeof(short), FRAME_SIZE, f1);
    if (feof(f1)) {
      if (!training) break;
      rewind(f1);
      fread(tmp, sizeof(short), FRAME_SIZE, f1);
      one_pass_completed = 1;
    }
    for (i=0;i<FRAME_SIZE;i++) E += tmp[i]*(float)tmp[i];
    if (training) {
      silent = E < 5000 || (last_silent && E < 20000);
      if (!last_silent && silent) {
        for (i=0;i<FRAME_SIZE;i++) savedX[i] = x[i];
      }
      if (last_silent && !silent) {
          for (i=0;i<FRAME_SIZE;i++) {
            float f = (float)i/FRAME_SIZE;
            tmp[i] = (int)floor(.5 + f*tmp[i] + (1-f)*savedX[i]);
          }
      }
      if (last_silent) {
        last_silent = silent;
        continue;
      }
      last_silent = silent;
    }
    if (count*FRAME_SIZE_5MS>=10000000 && one_pass_completed) break;
    if (training && ++gain_change_count > 2821) {
      float tmp;
      speech_gain = pow(10., (-20+(rand()%40))/20.);
      if (rand()%20==0) speech_gain *= .01;
      if (rand()%100==0) speech_gain = 0;
      gain_change_count = 0;
      rand_resp(a_sig, b_sig);
      tmp = (float)rand()/RAND_MAX;
      noise_std = 4*tmp*tmp;
    }
    biquad(x, mem_hp_x, x, b_hp, a_hp, FRAME_SIZE);
    biquad(x, mem_resp_x, x, b_sig, a_sig, FRAME_SIZE);
    preemphasis(x, &mem_preemph, x, PREEMPHASIS, FRAME_SIZE);
    for (i=0;i<FRAME_SIZE;i++) {
      float g;
      float f = (float)i/FRAME_SIZE;
      g = f*speech_gain + (1-f)*old_speech_gain;
      x[i] *= g;
    }
    for (i=0;i<FRAME_SIZE;i++) x[i] += rand()/(float)RAND_MAX - .5;
    /* PCM is delayed by 1/2 frame to make the features centered on the frames. */
    for (i=0;i<FRAME_SIZE-TRAINING_OFFSET;i++) pcm[i+TRAINING_OFFSET] = float2short(x[i]);
    compute_frame_features(st, x);

    RNN_COPY(&pcmbuf[st->pcount*FRAME_SIZE], pcm, FRAME_SIZE);
    if (fpcm) {
        compute_noise(&noisebuf[st->pcount*FRAME_SIZE], noise_std);
    }
    st->pcount++;
    /* Running on groups of 4 frames. */
    if (st->pcount == 4) {
      process_superframe(st, ffeat, encode, quantize);
      if (fpcm) write_audio(st, pcmbuf, noisebuf, fpcm);
      st->pcount = 0;
    }
    //if (fpcm) fwrite(pcm, sizeof(short), FRAME_SIZE, fpcm);
    for (i=0;i<TRAINING_OFFSET;i++) pcm[i] = float2short(x[i+FRAME_SIZE-TRAINING_OFFSET]);
    old_speech_gain = speech_gain;
    count++;
  }
  fclose(f1);
  fclose(ffeat);
  if (fpcm) fclose(fpcm);
  rnnoise_destroy(st);
  return 0;
}

