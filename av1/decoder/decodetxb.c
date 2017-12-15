/*
 * Copyright (c) 2017, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include "aom_ports/mem.h"
#include "av1/common/scan.h"
#include "av1/common/idct.h"
#include "av1/common/txb_common.h"
#include "av1/decoder/decodemv.h"
#include "av1/decoder/decodetxb.h"
#include "av1/decoder/symbolrate.h"

#define ACCT_STR __func__

static int read_golomb(MACROBLOCKD *xd, aom_reader *r, FRAME_COUNTS *counts) {
#if !CONFIG_SYMBOLRATE
  (void)counts;
#endif
  int x = 1;
  int length = 0;
  int i = 0;

  while (!i) {
    i = av1_read_record_bit(counts, r, ACCT_STR);
    ++length;
    if (length >= 32) {
      aom_internal_error(xd->error_info, AOM_CODEC_CORRUPT_FRAME,
                         "Invalid length in read_golomb");
      break;
    }
  }

  for (i = 0; i < length - 1; ++i) {
    x <<= 1;
    x += av1_read_record_bit(counts, r, ACCT_STR);
  }

  return x - 1;
}

static INLINE int rec_eob_pos(const int eob_token, const int extra) {
  int eob = k_eob_group_start[eob_token];
  if (eob > 2) {
    eob += extra;
  }
  return eob;
}

uint8_t av1_read_coeffs_txb(const AV1_COMMON *const cm, MACROBLOCKD *const xd,
                            aom_reader *const r, const int blk_row,
                            const int blk_col, const int plane,
#if CONFIG_NEW_QUANT
                            dequant_val_type_nuq *dq_val,
#endif  // CONFIG_NEW_QUANT
                            const TXB_CTX *const txb_ctx, const TX_SIZE tx_size,
                            int16_t *const max_scan_line, int *const eob) {
  FRAME_CONTEXT *const ec_ctx = xd->tile_ctx;
#if TXCOEFF_TIMER
  FRAME_COUNTS *const counts = NULL;
#else
  FRAME_COUNTS *const counts = xd->counts;
#endif
  const TX_SIZE txs_ctx = get_txsize_entropy_ctx(tx_size);
  const PLANE_TYPE plane_type = get_plane_type(plane);
  MB_MODE_INFO *const mbmi = &xd->mi[0]->mbmi;
  const int seg_eob = av1_get_max_eob(tx_size);
  int c = 0, v = 0;
  int num_updates = 0;
  struct macroblockd_plane *const pd = &xd->plane[plane];
  const int16_t *const dequant = pd->seg_dequant_QTX[mbmi->segment_id];
  tran_low_t *const tcoeffs = pd->dqcoeff;
#if CONFIG_NEW_QUANT
  const tran_low_t *dqv_val = &dq_val[0][0];
#endif  // CONFIG_NEW_QUANT
#if !CONFIG_DAALA_TX
  const int shift = av1_get_tx_scale(tx_size);
#endif
  const int bwl = get_txb_bwl(tx_size);
  const int width = get_txb_wide(tx_size);
  const int height = get_txb_high(tx_size);
  int cul_level = 0;
  uint8_t levels_buf[TX_PAD_2D];
  uint8_t *const levels = set_levels(levels_buf, width);
  DECLARE_ALIGNED(16, uint8_t, level_counts[MAX_TX_SQUARE]);
  int8_t signs[MAX_TX_SQUARE];
  uint16_t update_pos[MAX_TX_SQUARE];

  const int all_zero = av1_read_record_bin(
      counts, r, ec_ctx->txb_skip_cdf[txs_ctx][txb_ctx->txb_skip_ctx], 2,
      ACCT_STR);
  // printf("txb_skip: %d %2d\n", txs_ctx, txb_ctx->txb_skip_ctx);
  if (xd->counts)
    ++xd->counts->txb_skip[txs_ctx][txb_ctx->txb_skip_ctx][all_zero];
  *eob = 0;
  if (all_zero) {
    *max_scan_line = 0;
#if CONFIG_TXK_SEL
    if (plane == 0)
      mbmi->txk_type[(blk_row << MAX_MIB_SIZE_LOG2) + blk_col] = DCT_DCT;
#endif
    return 0;
  }

  memset(levels_buf, 0,
         sizeof(*levels_buf) *
             ((width + TX_PAD_HOR) * (height + TX_PAD_VER) + TX_PAD_END));

  (void)blk_row;
  (void)blk_col;
#if CONFIG_TXK_SEL
  av1_read_tx_type(cm, xd, blk_row, blk_col, plane, tx_size, r);
#endif
  const TX_TYPE tx_type =
      av1_get_tx_type(plane_type, xd, blk_row, blk_col, tx_size);
  const SCAN_ORDER *const scan_order = get_scan(cm, tx_size, tx_type, mbmi);
  const int16_t *const scan = scan_order->scan;
  int dummy;
  const int max_eob_pt = get_eob_pos_token(seg_eob, &dummy);
  int eob_extra = 0;
  int eob_pt = 1;

  for (eob_pt = 1; eob_pt < max_eob_pt; eob_pt++) {
    const int eob_pos_ctx = av1_get_eob_pos_ctx(tx_type, eob_pt);
    const int is_equal = av1_read_record_bin(
        counts, r, ec_ctx->eob_flag_cdf[txs_ctx][plane_type][eob_pos_ctx], 2,
        ACCT_STR);
    // printf("eob_flag_cdf: %d %d %2d\n", txs_ctx, plane_type, eob_pos_ctx);
    // aom_read_symbol(r,
    // ec_ctx->eob_flag_cdf[AOMMIN(txs_ctx,3)][plane_type][eob_pos_ctx], 2,
    // ACCT_STR);
    if (counts) ++counts->eob_flag[txs_ctx][plane_type][eob_pos_ctx][is_equal];

    if (is_equal) {
      break;
    }
  }

  // printf("Dec: ");
  if (k_eob_offset_bits[eob_pt] > 0) {
    int bit = av1_read_record_bin(
        counts, r, ec_ctx->eob_extra_cdf[txs_ctx][plane_type][eob_pt], 2,
        ACCT_STR);
    // printf("eob_extra_cdf: %d %d %2d\n", txs_ctx, plane_type, eob_pt);
    if (counts) ++counts->eob_extra[txs_ctx][plane_type][eob_pt][bit];
    if (bit) {
      eob_extra += (1 << (k_eob_offset_bits[eob_pt] - 1));
    }

    for (int i = 1; i < k_eob_offset_bits[eob_pt]; i++) {
      bit = av1_read_record_bit(counts, r, ACCT_STR);
      // printf("eob_bit:\n");
      if (bit) {
        eob_extra += (1 << (k_eob_offset_bits[eob_pt] - 1 - i));
      }
      //  printf("%d ", bit);
    }
  }
  *eob = rec_eob_pos(eob_pt, eob_extra);
  // printf("=>[%d, %d], (%d, %d)\n", seg_eob, *eob, eob_pt, eob_extra);

  for (int i = 0; i < *eob; ++i) {
    c = *eob - 1 - i;
    const int pos = scan[c];
#if CONFIG_LV_MAP_MULTI
    const int coeff_ctx = get_nz_map_ctx(levels, pos, bwl, height, c,
                                         c == *eob - 1, tx_size, tx_type);
    aom_cdf_prob *cdf;
    int nsymbs;
    if (c == *eob - 1) {
      cdf = ec_ctx->coeff_base_eob_cdf[txs_ctx][plane_type][coeff_ctx];
      nsymbs = 3;
    } else {
      cdf = ec_ctx->coeff_base_cdf[txs_ctx][plane_type][coeff_ctx];
      nsymbs = 4;
    }
    const int level = av1_read_record_symbol(counts, r, cdf, nsymbs, ACCT_STR) +
                      (c == *eob - 1);
    if (counts) {
      if (c == *eob - 1) {
        ++counts
              ->coeff_base_eob_multi[txs_ctx][plane_type][coeff_ctx][level - 1];
      } else {
        ++counts->coeff_base_multi[txs_ctx][plane_type][coeff_ctx][level];
      }
    }

    // printf("base_cdf: %d %d %2d\n", txs_ctx, plane_type, coeff_ctx);
    // printf("base_cdf: %d %d %2d : %3d %3d %3d\n", txs_ctx, plane_type,
    // coeff_ctx,
    //            ec_ctx->coeff_base_cdf[txs_ctx][plane_type][coeff_ctx][0]>>7,
    //            ec_ctx->coeff_base_cdf[txs_ctx][plane_type][coeff_ctx][1]>>7,
    //            ec_ctx->coeff_base_cdf[txs_ctx][plane_type][coeff_ctx][2]>>7);
    if (level) {
      levels[get_padded_idx(pos, bwl)] = level;
      *max_scan_line = AOMMAX(*max_scan_line, pos);
      if (level < 3) {
        cul_level += level;
#if CONFIG_NEW_QUANT
        dqv_val = &dq_val[pos != 0][0];
        v = av1_dequant_abscoeff_nuq(level, dequant[!!c], dqv_val);
#if !CONFIG_DAALA_TX
        v = shift ? ROUND_POWER_OF_TWO(v, shift) : v;
#endif  // !CONFIG_DAALA_TX
#else
        v = level * dequant[!!c];
#if !CONFIG_DAALA_TX
        v = v >> shift;
#endif  // !CONFIG_DAALA_TX
#endif  // CONFIG_NEW_QUANT
        tcoeffs[pos] = v;
      } else {
        update_pos[num_updates++] = pos;
      }
    }
#else
    int is_nz;
    const int coeff_ctx = get_nz_map_ctx(levels, pos, bwl, tx_size, tx_type);

    if (c < *eob - 1) {
      is_nz = av1_read_record_bin(
          counts, r, ec_ctx->nz_map_cdf[txs_ctx][plane_type][coeff_ctx], 2,
          ACCT_STR);
    } else {
      is_nz = 1;
    }

    if (is_nz) {
      int k;
      for (k = 0; k < NUM_BASE_LEVELS; ++k) {
        const int ctx = coeff_ctx;
        const int is_k = av1_read_record_bin(
            counts, r, ec_ctx->coeff_base_cdf[txs_ctx][plane_type][k][ctx], 2,
            ACCT_STR);
        if (counts) ++counts->coeff_base[txs_ctx][plane_type][k][ctx][is_k];

        // semantic: is_k = 1 if level > (k+1)
        if (is_k == 0) {
          cul_level += k + 1;
#if CONFIG_NEW_QUANT
          dqv_val = &dq_val[pos != 0][0];
          v = av1_dequant_abscoeff_nuq(k + 1, dequant[!!c], dqv_val);
#if !CONFIG_DAALA_TX
          v = shift ? ROUND_POWER_OF_TWO(v, shift) : v;
#endif  // !CONFIG_DAALA_TX
#else
          v = (k + 1) * dequant[!!c];
#if !CONFIG_DAALA_TX
          v = v >> shift;
#endif  // !CONFIG_DAALA_TX
#endif  // CONFIG_NEW_QUANT
          tcoeffs[pos] = v;
          break;
        }
      }
      levels[get_padded_idx(pos, bwl)] = k + 1;
      *max_scan_line = AOMMAX(*max_scan_line, pos);
      if (k == NUM_BASE_LEVELS) {
        update_pos[num_updates++] = pos;
      }
    }
#endif
  }

  // Loop to decode all signs in the transform block,
  // starting with the sign of the DC (if applicable)
  for (c = 0; c < *eob; ++c) {
    const int pos = scan[c];
    int8_t *const sign = &signs[pos];
    if (levels[get_padded_idx(pos, bwl)] == 0) continue;
    if (c == 0) {
      const int dc_sign_ctx = txb_ctx->dc_sign_ctx;
#if LV_MAP_PROB
      *sign = av1_read_record_bin(
          counts, r, ec_ctx->dc_sign_cdf[plane_type][dc_sign_ctx], 2, ACCT_STR);
// printf("dc_sign: %d %d\n", plane_type, dc_sign_ctx);
#else
      *sign = aom_read(r, ec_ctx->dc_sign[plane_type][dc_sign_ctx], ACCT_STR);
#endif
      if (counts) ++counts->dc_sign[plane_type][dc_sign_ctx][*sign];
    } else {
      *sign = av1_read_record_bit(counts, r, ACCT_STR);
    }
    if (*sign) tcoeffs[pos] = -tcoeffs[pos];
  }

  if (num_updates) {
#if !CONFIG_LV_MAP_MULTI
    av1_get_br_level_counts(levels, width, height, level_counts);
#endif
    for (c = 0; c < num_updates; ++c) {
      const int pos = update_pos[c];
      uint8_t *const level = &levels[get_padded_idx(pos, bwl)];
      int idx;
      int ctx;

      assert(*level > NUM_BASE_LEVELS);

#if !CONFIG_LV_MAP_MULTI
      ctx = get_br_ctx(levels, pos, bwl, level_counts[pos]);
#endif

#if CONFIG_LV_MAP_MULTI
#if USE_CAUSAL_BR_CTX
      ctx = get_br_ctx(levels, pos, bwl, level_counts[pos], tx_type);
#else
      ctx = get_br_ctx(levels, pos, bwl, level_counts[pos]);
#endif
      for (idx = 0; idx < COEFF_BASE_RANGE / (BR_CDF_SIZE - 1); ++idx) {
        int k = av1_read_record_symbol(
            counts, r,
            ec_ctx->coeff_br_cdf[AOMMIN(txs_ctx, TX_32X32)][plane_type][ctx],
            BR_CDF_SIZE, ACCT_STR);
        *level += k;
        if (counts) {
          for (int lps = 0; lps < BR_CDF_SIZE - 1; lps++) {
            ++counts->coeff_lps[AOMMIN(txs_ctx, TX_32X32)][plane_type][lps][ctx]
                               [lps == k];
            if (lps == k) break;
          }
          ++counts->coeff_lps_multi[AOMMIN(txs_ctx, TX_32X32)][plane_type][ctx]
                                   [k];
        }
        if (k < BR_CDF_SIZE - 1) break;
      }
      if (*level <= NUM_BASE_LEVELS + COEFF_BASE_RANGE) {
        cul_level += *level;
        tran_low_t t;
#if CONFIG_NEW_QUANT
        dqv_val = &dq_val[pos != 0][0];
        t = av1_dequant_abscoeff_nuq(*level, dequant[!!pos], dqv_val);
#if !CONFIG_DAALA_TX
        t = shift ? ROUND_POWER_OF_TWO(t, shift) : t;
#endif  // !CONFIG_DAALA_TX
#else
        t = *level * dequant[!!pos];
#if !CONFIG_DAALA_TX
        t = t >> shift;
#endif  // !CONFIG_DAALA_TX
#endif  // CONFIG_NEW_QUANT
        if (signs[pos]) t = -t;
        tcoeffs[pos] = t;
        continue;
      }
#else
      for (idx = 0; idx < BASE_RANGE_SETS; ++idx) {
        // printf("br: %d %d %d %d\n", txs_ctx, plane_type, idx, ctx);
        if (av1_read_record_bin(
                counts, r, ec_ctx->coeff_br_cdf[txs_ctx][plane_type][idx][ctx],
                2, ACCT_STR)) {
          const int extra_bits = (1 << br_extra_bits[idx]) - 1;
          //        int br_offset = aom_read_literal(r, extra_bits, ACCT_STR);
          int br_offset = 0;
          int tok;
          if (counts) ++counts->coeff_br[txs_ctx][plane_type][idx][ctx][1];
          for (tok = 0; tok < extra_bits; ++tok) {
            if (av1_read_record_bin(
                    counts, r, ec_ctx->coeff_lps_cdf[txs_ctx][plane_type][ctx],
                    2, ACCT_STR)) {
              br_offset = tok;
              if (counts) ++counts->coeff_lps[txs_ctx][plane_type][ctx][1];
              break;
            }
            if (counts) ++counts->coeff_lps[txs_ctx][plane_type][ctx][0];
          }
          if (tok == extra_bits) br_offset = extra_bits;

          const int br_base = br_index_to_coeff[idx];

          *level = NUM_BASE_LEVELS + 1 + br_base + br_offset;
          cul_level += *level;
          tran_high_t t;
#if CONFIG_NEW_QUANT
          dqv_val = &dq_val[pos != 0][0];
          t = av1_dequant_abscoeff_nuq(*level, dequant[!!pos], dqv_val);
#if !CONFIG_DAALA_TX
          t = shift ? ROUND_POWER_OF_TWO(t, shift) : t;
#endif  // !CONFIG_DAALA_TX
#else
          t = *level * dequant[!!pos];
#if !CONFIG_DAALA_TX
          t = t >> shift;
#endif  // !CONFIG_DAALA_TX
#endif  // CONFIG_NEW_QUANT
          if (signs[pos]) t = -t;
          tcoeffs[pos] = (tran_low_t)t;
          break;
        }
        if (counts) ++counts->coeff_br[txs_ctx][plane_type][idx][ctx][0];
      }

      if (idx < BASE_RANGE_SETS) continue;
#endif
      // decode 0-th order Golomb code
      *level = COEFF_BASE_RANGE + 1 + NUM_BASE_LEVELS;
      // Save golomb in tcoeffs because adding it to level may incur overflow
      tran_high_t t = *level + read_golomb(xd, r, counts);
      cul_level += (int)t;
#if CONFIG_NEW_QUANT
      dqv_val = &dq_val[pos != 0][0];
      t = av1_dequant_abscoeff_nuq(t, dequant[!!pos], dqv_val);
#if !CONFIG_DAALA_TX
      t = shift ? ROUND_POWER_OF_TWO(t, shift) : t;
#endif  // !CONFIG_DAALA_TX
#else
      t = t * dequant[!!pos];
#if !CONFIG_DAALA_TX
      t = t >> shift;
#endif  // !CONFIG_DAALA_TX
#endif  // CONFIG_NEW_QUANT
      if (signs[pos]) t = -t;
      tcoeffs[pos] = (tran_low_t)t;
    }
  }

  cul_level = AOMMIN(63, cul_level);

  // DC value
  set_dc_sign(&cul_level, tcoeffs[0]);

  return cul_level;
}

uint8_t av1_read_coeffs_txb_facade(const AV1_COMMON *const cm,
                                   MACROBLOCKD *const xd, aom_reader *const r,
                                   const int row, const int col,
                                   const int plane, const TX_SIZE tx_size,
                                   int16_t *const max_scan_line,
                                   int *const eob) {
  MB_MODE_INFO *const mbmi = &xd->mi[0]->mbmi;
  struct macroblockd_plane *const pd = &xd->plane[plane];

  const BLOCK_SIZE bsize = mbmi->sb_type;
  const BLOCK_SIZE plane_bsize = get_plane_block_size(bsize, pd);
#if CONFIG_NEW_QUANT
  const int seg_id = mbmi->segment_id;
  const int ref = is_inter_block(mbmi);
  int dq = get_dq_profile(xd->qindex[seg_id], ref, pd->plane_type);
#endif  //  CONFIG_NEW_QUANT

  TXB_CTX txb_ctx;
  get_txb_ctx(plane_bsize, tx_size, plane, pd->above_context + col,
              pd->left_context + row, &txb_ctx);
  uint8_t cul_level =
      av1_read_coeffs_txb(cm, xd, r, row, col, plane,
#if CONFIG_NEW_QUANT
                          pd->seg_dequant_nuq_QTX[seg_id][dq],
#endif  // CONFIG_NEW_QUANT
                          &txb_ctx, tx_size, max_scan_line, eob);
  av1_set_contexts(xd, pd, plane, tx_size, cul_level, col, row);
  return cul_level;
}
