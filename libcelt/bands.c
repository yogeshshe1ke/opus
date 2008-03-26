/* (C) 2007-2008 Jean-Marc Valin, CSIRO
*/
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:
   
   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   
   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
   
   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.
   
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

#include <math.h>
#include "bands.h"
#include "modes.h"
#include "vq.h"
#include "cwrs.h"
#include "stack_alloc.h"
#include "os_support.h"
#include "mathops.h"

void exp_rotation(celt_norm_t *X, int len, int dir, int stride, int iter)
{
   int i, k;
   celt_word16_t c, s;
   /* Equivalent to cos(.3) and sin(.3) */
   c = QCONST16(0.95534,15);
   s = dir*QCONST16(0.29552,15);
   for (k=0;k<iter;k++)
   {
      /* We could use MULT16_16_P15 instead of MULT16_16_Q15 for more accuracy, 
         but at this point, I really don't think it's necessary */
      for (i=0;i<len-stride;i++)
      {
         celt_norm_t x1, x2;
         x1 = X[i];
         x2 = X[i+stride];
         X[i] = MULT16_16_Q15(c,x1) - MULT16_16_Q15(s,x2);
         X[i+stride] = MULT16_16_Q15(c,x2) + MULT16_16_Q15(s,x1);
      }
      for (i=len-2*stride-1;i>=0;i--)
      {
         celt_norm_t x1, x2;
         x1 = X[i];
         x2 = X[i+stride];
         X[i] = MULT16_16_Q15(c,x1) - MULT16_16_Q15(s,x2);
         X[i+stride] = MULT16_16_Q15(c,x2) + MULT16_16_Q15(s,x1);
      }
   }
}


const celt_word16_t sqrtC_1[2] = {QCONST16(1.f, 14), QCONST16(1.414214f, 14)};

#ifdef FIXED_POINT
/* Compute the amplitude (sqrt energy) in each of the bands */
void compute_band_energies(const CELTMode *m, const celt_sig_t *X, celt_ener_t *bank)
{
   int i, c, B, C;
   const celt_int16_t *eBands = m->eBands;
   B = m->nbMdctBlocks;
   C = m->nbChannels;
   for (c=0;c<C;c++)
   {
      for (i=0;i<m->nbEBands;i++)
      {
         int j;
         celt_word32_t maxval=0;
         celt_word32_t sum = 0;
         for (j=B*eBands[i];j<B*eBands[i+1];j++)
            maxval = MAX32(maxval, ABS32(X[j*C+c]));
         if (maxval > 0)
         {
            int shift = celt_ilog2(maxval)-10;
            for (j=B*eBands[i];j<B*eBands[i+1];j++)
               sum += MULT16_16(EXTRACT16(VSHR32(X[j*C+c],shift)),EXTRACT16(VSHR32(X[j*C+c],shift)));
            /* We're adding one here to make damn sure we never end up with a pitch vector that's
               larger than unity norm */
            bank[i*C+c] = EPSILON+VSHR32(EXTEND32(celt_sqrt(sum)),-shift);
         } else {
            bank[i*C+c] = EPSILON;
         }
         /*printf ("%f ", bank[i*C+c]);*/
      }
   }
   /*printf ("\n");*/
}

/* Normalise each band such that the energy is one. */
void normalise_bands(const CELTMode *m, const celt_sig_t * restrict freq, celt_norm_t * restrict X, const celt_ener_t *bank)
{
   int i, c, B, C;
   const celt_int16_t *eBands = m->eBands;
   B = m->nbMdctBlocks;
   C = m->nbChannels;
   for (c=0;c<C;c++)
   {
      for (i=0;i<m->nbEBands;i++)
      {
         celt_word16_t g;
         int j,shift;
         celt_word16_t E;
         shift = celt_zlog2(bank[i*C+c])-13;
         E = VSHR32(bank[i*C+c], shift);
         g = EXTRACT16(celt_rcp(SHR32(MULT16_16(E,sqrtC_1[C-1]),11)));
         for (j=B*eBands[i];j<B*eBands[i+1];j++)
            X[j*C+c] = MULT16_16_Q14(VSHR32(freq[j*C+c],shift),g);
      }
   }
   for (i=B*C*eBands[m->nbEBands];i<B*C*eBands[m->nbEBands+1];i++)
      X[i] = 0;
}

void renormalise_bands(const CELTMode *m, celt_norm_t * restrict X)
{
   int i;
   VARDECL(celt_ener_t, tmpE);
   VARDECL(celt_sig_t, freq);
   SAVE_STACK;
   ALLOC(tmpE, m->nbEBands*m->nbChannels, celt_ener_t);
   ALLOC(freq, m->nbMdctBlocks*m->nbChannels*m->eBands[m->nbEBands+1], celt_sig_t);
   for (i=0;i<m->nbMdctBlocks*m->nbChannels*m->eBands[m->nbEBands+1];i++)
      freq[i] = SHL32(EXTEND32(X[i]), 10);
   compute_band_energies(m, freq, tmpE);
   normalise_bands(m, freq, X, tmpE);
   RESTORE_STACK;
}
#else
/* Compute the amplitude (sqrt energy) in each of the bands */
void compute_band_energies(const CELTMode *m, const celt_sig_t *X, celt_ener_t *bank)
{
   int i, c, B, C;
   const celt_int16_t *eBands = m->eBands;
   B = m->nbMdctBlocks;
   C = m->nbChannels;
   for (c=0;c<C;c++)
   {
      for (i=0;i<m->nbEBands;i++)
      {
         int j;
         celt_word32_t sum = 1e-10;
         for (j=B*eBands[i];j<B*eBands[i+1];j++)
            sum += X[j*C+c]*X[j*C+c];
         bank[i*C+c] = sqrt(sum);
         /*printf ("%f ", bank[i*C+c]);*/
      }
   }
   /*printf ("\n");*/
}

/* Normalise each band such that the energy is one. */
void normalise_bands(const CELTMode *m, const celt_sig_t * restrict freq, celt_norm_t * restrict X, const celt_ener_t *bank)
{
   int i, c, B, C;
   const celt_int16_t *eBands = m->eBands;
   B = m->nbMdctBlocks;
   C = m->nbChannels;
   for (c=0;c<C;c++)
   {
      for (i=0;i<m->nbEBands;i++)
      {
         int j;
         celt_word16_t g = 1.f/(1e-10+bank[i*C+c]*sqrt(C));
         for (j=B*eBands[i];j<B*eBands[i+1];j++)
            X[j*C+c] = freq[j*C+c]*g;
      }
   }
   for (i=B*C*eBands[m->nbEBands];i<B*C*eBands[m->nbEBands+1];i++)
      X[i] = 0;
}

void renormalise_bands(const CELTMode *m, celt_norm_t * restrict X)
{
   VARDECL(celt_ener_t, tmpE);
   SAVE_STACK;
   ALLOC(tmpE, m->nbEBands*m->nbChannels, celt_ener_t);
   compute_band_energies(m, X, tmpE);
   normalise_bands(m, X, X, tmpE);
   RESTORE_STACK;
}
#endif

/* De-normalise the energy to produce the synthesis from the unit-energy bands */
void denormalise_bands(const CELTMode *m, const celt_norm_t * restrict X, celt_sig_t * restrict freq, const celt_ener_t *bank)
{
   int i, c, B, C;
   const celt_int16_t *eBands = m->eBands;
   B = m->nbMdctBlocks;
   C = m->nbChannels;
   if (C>2)
      celt_fatal("denormalise_bands() not implemented for >2 channels");
   for (c=0;c<C;c++)
   {
      for (i=0;i<m->nbEBands;i++)
      {
         int j;
         celt_word32_t g = MULT16_32_Q14(sqrtC_1[C-1],bank[i*C+c]);
         for (j=B*eBands[i];j<B*eBands[i+1];j++)
            freq[j*C+c] = MULT16_32_Q14(X[j*C+c], g);
      }
   }
   for (i=B*C*eBands[m->nbEBands];i<B*C*eBands[m->nbEBands+1];i++)
      freq[i] = 0;
}


/* Compute the best gain for each "pitch band" */
void compute_pitch_gain(const CELTMode *m, const celt_norm_t *X, const celt_norm_t *P, celt_pgain_t *gains)
{
   int i, B;
   const celt_int16_t *pBands = m->pBands;
   B = m->nbMdctBlocks*m->nbChannels;
   
   for (i=0;i<m->nbPBands;i++)
   {
      celt_word32_t Sxy=0, Sxx=0;
      int j;
      /* We know we're not going to overflow because Sxx can't be more than 1 (Q28) */
      for (j=B*pBands[i];j<B*pBands[i+1];j++)
      {
         Sxy = MAC16_16(Sxy, X[j], P[j]);
         Sxx = MAC16_16(Sxx, X[j], X[j]);
      }
      /* No negative gain allowed */
      if (Sxy < 0)
         Sxy = 0;
      /* Not sure how that would happen, just making sure */
      if (Sxy > Sxx)
         Sxy = Sxx;
      /* We need to be a bit conservative (multiply gain by 0.9), otherwise the
         residual doesn't quantise well */
      Sxy = MULT16_32_Q15(QCONST16(.9f, 15), Sxy);
      /* gain = Sxy/Sxx */
      gains[i] = EXTRACT16(celt_div(Sxy,ADD32(SHR32(Sxx, PGAIN_SHIFT),EPSILON)));
      /*printf ("%f ", 1-sqrt(1-gain*gain));*/
   }
   /*if(rand()%10==0)
   {
      for (i=0;i<m->nbPBands;i++)
         printf ("%f ", 1-sqrt(1-gains[i]*gains[i]));
      printf ("\n");
   }*/
}

/* Apply the (quantised) gain to each "pitch band" */
void pitch_quant_bands(const CELTMode *m, celt_norm_t * restrict P, const celt_pgain_t * restrict gains)
{
   int i, B;
   const celt_int16_t *pBands = m->pBands;
   B = m->nbMdctBlocks*m->nbChannels;
   for (i=0;i<m->nbPBands;i++)
   {
      int j;
      for (j=B*pBands[i];j<B*pBands[i+1];j++)
         P[j] = MULT16_16_Q15(gains[i], P[j]);
      /*printf ("%f ", gain);*/
   }
   for (i=B*pBands[m->nbPBands];i<B*pBands[m->nbPBands+1];i++)
      P[i] = 0;
}


/* Quantisation of the residual */
void quant_bands(const CELTMode *m, celt_norm_t * restrict X, celt_norm_t *P, celt_mask_t *W, int total_bits, ec_enc *enc)
{
   int i, j, B, bits;
   const celt_int16_t *eBands = m->eBands;
   celt_norm_t * restrict norm;
   VARDECL(celt_norm_t, _norm);
   VARDECL(int, pulses);
   VARDECL(int, offsets);
   SAVE_STACK;

   B = m->nbMdctBlocks*m->nbChannels;
   
   ALLOC(_norm, B*eBands[m->nbEBands+1], celt_norm_t);
   ALLOC(pulses, m->nbEBands, int);
   ALLOC(offsets, m->nbEBands, int);
   norm = _norm;

   for (i=0;i<m->nbEBands;i++)
      offsets[i] = 0;
   /* Use a single-bit margin to guard against overrunning (make sure it's enough) */
   bits = total_bits - ec_enc_tell(enc, 0) - 1;
   compute_allocation(m, offsets, bits, pulses);
   
   /*printf("bits left: %d\n", bits);
   for (i=0;i<m->nbEBands;i++)
      printf ("%d ", pulses[i]);
   printf ("\n");*/
   /*printf ("%d %d\n", ec_enc_tell(enc, 0), compute_allocation(m, m->nbPulses));*/
   for (i=0;i<m->nbEBands;i++)
   {
      int q;
      celt_word16_t n;
      q = pulses[i];
      n = SHL16(celt_sqrt(B*(eBands[i+1]-eBands[i])),11);

      /* If pitch isn't available, use intra-frame prediction */
      if (eBands[i] >= m->pitchEnd || q<=0)
      {
         q -= 1;
         if (q<0)
            intra_fold(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), norm, P+B*eBands[i], B, eBands[i], eBands[m->nbEBands+1]);
         else
            intra_prediction(X+B*eBands[i], W+B*eBands[i], B*(eBands[i+1]-eBands[i]), q, norm, P+B*eBands[i], B, eBands[i], enc);
      }
      
      if (q > 0)
      {
         int nb_rotations = (B*(eBands[i+1]-eBands[i])+4*q)/(8*q);
         exp_rotation(P+B*eBands[i], B*(eBands[i+1]-eBands[i]), -1, B, nb_rotations);
         exp_rotation(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), -1, B, nb_rotations);
         alg_quant(X+B*eBands[i], W+B*eBands[i], B*(eBands[i+1]-eBands[i]), q, P+B*eBands[i], enc);
         exp_rotation(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), 1, B, nb_rotations);
      }
      for (j=B*eBands[i];j<B*eBands[i+1];j++)
         norm[j] = MULT16_16_Q15(n,X[j]);
   }
   for (i=B*eBands[m->nbEBands];i<B*eBands[m->nbEBands+1];i++)
      X[i] = 0;
   RESTORE_STACK;
}

/* Decoding of the residual */
void unquant_bands(const CELTMode *m, celt_norm_t * restrict X, celt_norm_t *P, int total_bits, ec_dec *dec)
{
   int i, j, B, bits;
   const celt_int16_t *eBands = m->eBands;
   celt_norm_t * restrict norm;
   VARDECL(celt_norm_t, _norm);
   VARDECL(int, pulses);
   VARDECL(int, offsets);
   SAVE_STACK;

   B = m->nbMdctBlocks*m->nbChannels;
   
   ALLOC(_norm, B*eBands[m->nbEBands+1], celt_norm_t);
   ALLOC(pulses, m->nbEBands, int);
   ALLOC(offsets, m->nbEBands, int);
   norm = _norm;

   for (i=0;i<m->nbEBands;i++)
      offsets[i] = 0;
   /* Use a single-bit margin to guard against overrunning (make sure it's enough) */
   bits = total_bits - ec_dec_tell(dec, 0) - 1;
   compute_allocation(m, offsets, bits, pulses);

   for (i=0;i<m->nbEBands;i++)
   {
      int q;
      celt_word16_t n;
      q = pulses[i];
      n = SHL16(celt_sqrt(B*(eBands[i+1]-eBands[i])),11);

      /* If pitch isn't available, use intra-frame prediction */
      if (eBands[i] >= m->pitchEnd || q<=0)
      {
         q -= 1;
         if (q<0)
            intra_fold(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), norm, P+B*eBands[i], B, eBands[i], eBands[m->nbEBands+1]);
         else
            intra_unquant(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), q, norm, P+B*eBands[i], B, eBands[i], dec);
      }
      
      if (q > 0)
      {
         int nb_rotations = (B*(eBands[i+1]-eBands[i])+4*q)/(8*q);
         exp_rotation(P+B*eBands[i], B*(eBands[i+1]-eBands[i]), -1, B, nb_rotations);
         alg_unquant(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), q, P+B*eBands[i], dec);
         exp_rotation(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), 1, B, nb_rotations);
      }
      for (j=B*eBands[i];j<B*eBands[i+1];j++)
         norm[j] = MULT16_16_Q15(n,X[j]);
   }
   for (i=B*eBands[m->nbEBands];i<B*eBands[m->nbEBands+1];i++)
      X[i] = 0;
   RESTORE_STACK;
}

void stereo_mix(const CELTMode *m, celt_norm_t *X, const celt_ener_t *bank, int dir)
{
   int i, B, C;
   const celt_int16_t *eBands = m->eBands;
   B = m->nbMdctBlocks;
   C = m->nbChannels;
   for (i=0;i<m->nbEBands;i++)
   {
      int j;
      celt_word16_t left, right;
      celt_word16_t a1, a2;
      celt_word16_t norm;
#ifdef FIXED_POINT
      int shift = celt_zlog2(MAX32(bank[i*C], bank[i*C+1]))-13;
#endif
      left = VSHR32(bank[i*C],shift);
      right = VSHR32(bank[i*C+1],shift);
      norm = EPSILON + celt_sqrt(EPSILON+MULT16_16(left,left)+MULT16_16(right,right));
      a1 = DIV32_16(SHL32(EXTEND32(left),14),norm);
      a2 = dir*DIV32_16(SHL32(EXTEND32(right),14),norm);
      for (j=B*eBands[i];j<B*eBands[i+1];j++)
      {
         celt_norm_t r, l;
         l = X[j*C];
         r = X[j*C+1];
         X[j*C] = MULT16_16_Q14(a1,l) + MULT16_16_Q14(a2,r);
         X[j*C+1] = MULT16_16_Q14(a1,r) - MULT16_16_Q14(a2,l);
      }
   }
   for (i=B*C*eBands[m->nbEBands];i<B*C*eBands[m->nbEBands+1];i++)
      X[i] = 0;

}
