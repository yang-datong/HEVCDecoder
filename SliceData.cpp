#include "SliceData.hpp"
#include "Cabac.hpp"
#include "Common.hpp"
#include "Frame.hpp"
#include "GOP.hpp"
#include "MacroBlock.hpp"
#include "PU.hpp"
#include "PictureBase.hpp"
#include "Type.hpp"
#include <cstdint>
#include <cstdlib>

/* 7.3.4 Slice data syntax */
int SliceData::slice_segment_data(BitStream &bitStream, PictureBase &picture,
                                  SPS &sps, PPS &pps) {
  /* 初始化类中的指针 */
  pic = &picture;
  header = pic->m_slice->slice_header;
  bs = &bitStream;
  m_sps = &sps;
  m_pps = &pps;
  m_pu = PU();
  //m_cu = CU();
  //m_tu = TU();

  if (cabac == nullptr) cabac = new Cabac(*bs, *pic);

  tab_ct_depth = new uint8_t[m_sps->min_cb_height * m_sps->min_cb_width]{0};

  //----------------------- 开始对Slice分割为MacroBlock进行处理 ----------------------------

  //int ctb_addr_ts = m_pps->CtbAddrRsToTs[header->slice_ctb_addr_rs];
  CtbAddrInTs = m_pps->CtbAddrRsToTs[header->slice_ctb_addr_rs];
  bool end_of_slice_segment_flag = false;
  do {
    // 编码顺序: 递增当前CTU的地址
    // 光栅顺序: 将编码顺序下的CTU地址转换为光栅顺序下的CTU地址
    CtbAddrInRs = m_pps->CtbAddrTsToRs[CtbAddrInTs];
    int32_t xCtb = (CtbAddrInRs % m_sps->PicWidthInCtbsY)
                   << m_sps->CtbLog2SizeY;
    int32_t yCtb = (CtbAddrInRs / m_sps->PicWidthInCtbsY)
                   << m_sps->CtbLog2SizeY;

    // CtbAddrInRs 是当前CTU在光栅顺序（Raster Scan）中的地址
    hls_decode_neighbour(xCtb, yCtb, CtbAddrInTs);
    std::cout << "xCtb:" << xCtb << std::endl;
    std::cout << "yCtb:" << yCtb << std::endl;

    // – 如果 CTU 是图块中的第一个 CTU，则以下规则适用：
    if (CtbAddrInTs == m_pps->CtbAddrRsToTs[header->slice_ctb_addr_rs]) {
      //9.3.2.6 Initialization process for the arithmetic decoding engine
      cabac->ff_initialization_decoding_engine();
      // 当前片段不是依赖片段,或者启用了 Tile 并且当前块与前一个块不在同一个 Tile 中，则初始化 CABAC 的状态
      if (header->dependent_slice_segment_flag == 0 ||
          (m_pps->tiles_enabled_flag &&
           m_pps->TileId[CtbAddrInTs] != m_pps->TileId[CtbAddrInTs - 1]))
        // – 上下文变量的初始化过程按照第 9.3.2.2 节的规定被调用。
        cabac->ff_initialization_context_variables(header);

      // – 变量 StatCoeff[ k ] 设置为等于 0，因为 k 的范围为 0 到 3（含）。
      for (int i = 0; i < 4; i++)
        StatCoeff[i] = 0;

      // – 按照第 9.3.2.3 节的规定调用调色板预测变量的初始化过程。 TODO 这里ffmpeg没有调用，为什么？ <24-12-14 09:06:21, YangJing>
      //cabac->initialization_palette_predictor_entries(m_sps, m_pps);
    } else {
      if (m_pps->tiles_enabled_flag &&
          m_pps->TileId[CtbAddrInTs] != m_pps->TileId[CtbAddrInTs - 1]) {
        std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__
                  << std::endl;
        exit(0);
      }

      if (m_pps->entropy_coding_sync_enabled_flag) {
        if (CtbAddrInTs % m_sps->ctb_width == 0) {
          std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__
                    << std::endl;
          exit(0);
        }
      }
    }

    // 解析当前CTU（编码树单元）的数据
    coding_tree_unit();

    end_of_slice_segment_flag = cabac->ff_decode_terminate(); // ae(v);

    // 当前片段还没有结束
    if (!end_of_slice_segment_flag) {
      // 启用了Tile（瓦片）,当前CTU的Tile ID与前一个CTU的Tile ID不同(跨越了Tile边界)
      if (m_pps->tiles_enabled_flag &&
          m_pps->TileId[CtbAddrInTs] != m_pps->TileId[CtbAddrInTs - 1])
        goto check;
      // 启用了熵编码同步,当前CTU位于图像的最左边，跨越了Tile边界
      else if (m_pps->entropy_coding_sync_enabled_flag &&
               (CtbAddrInRs % m_sps->PicWidthInCtbsY == 0 ||
                m_pps->TileId[CtbAddrInTs] !=
                    m_pps->TileId[m_pps->CtbAddrRsToTs[CtbAddrInRs - 1]]))
        goto check;

    check:
      int end_of_subset_one_bit = cabac->ff_decode_terminate(); // ae(v);
      bs->byte_alignment();
    }
    CtbAddrInTs++;
    /* TODO YangJing 暂时先解码一个CTU验证CABAC <25-01-01 02:05:15> */
    exit(0);
  } while (!end_of_slice_segment_flag);
  return 0;
}

static const unsigned av_mod_uintp2_c(unsigned a, unsigned p) {
  return a & ((1U << p) - 1);
}

static inline int av_size_mult(size_t a, size_t b, size_t *r) {
  size_t t = a * b;
  /* Hack inspired from glibc: don't try the division if nelem and elsize
     * are both less than sqrt(SIZE_MAX). */
  if ((a | b) >= ((size_t)1 << (sizeof(size_t) * 4)) && a && t / a != b)
    return -1;
  *r = t;
  return 0;
}

#define BOUNDARY_LEFT_SLICE (1 << 0)
#define BOUNDARY_LEFT_TILE (1 << 1)
#define BOUNDARY_UPPER_SLICE (1 << 2)
#define BOUNDARY_UPPER_TILE (1 << 3)
void SliceData::hls_decode_neighbour(int x_ctb, int y_ctb, int ctb_addr_ts) {
  int ctb_size = 1 << m_sps->CtbLog2SizeY;
  int ctb_addr_rs = m_pps->CtbAddrTsToRs[ctb_addr_ts];
  int ctb_addr_in_slice = ctb_addr_rs - header->SliceAddrRs;
  tab_slice_address[ctb_addr_rs] = header->SliceAddrRs;

  if (m_pps->entropy_coding_sync_enabled_flag) {
    if (x_ctb == 0 && (y_ctb & (ctb_size - 1)) == 0) first_qp_group = 1;
    end_of_tiles_x = m_sps->width;
  } else if (m_pps->tiles_enabled_flag) {
    if (ctb_addr_ts &&
        m_pps->TileId[ctb_addr_ts] != m_pps->TileId[ctb_addr_ts - 1]) {
      int idxX = m_pps->col_idxX[x_ctb >> m_sps->CtbLog2SizeY];
      end_of_tiles_x = x_ctb + (m_pps->colWidth[idxX] << m_sps->CtbLog2SizeY);
      first_qp_group = 1;
    }
  } else {
    end_of_tiles_x = m_sps->width;
  }

  int end_of_tiles_y = MIN(y_ctb + ctb_size, m_sps->height);

  int boundary_flags = 0;
  if (m_pps->tiles_enabled_flag) {
    if (x_ctb > 0 && m_pps->TileId[ctb_addr_ts] !=
                         m_pps->TileId[m_pps->CtbAddrRsToTs[ctb_addr_rs - 1]])
      boundary_flags |= BOUNDARY_LEFT_TILE;
    if (x_ctb > 0 &&
        tab_slice_address[ctb_addr_rs] != tab_slice_address[ctb_addr_rs - 1])
      boundary_flags |= BOUNDARY_LEFT_SLICE;
    if (y_ctb > 0 &&
        m_pps->TileId[ctb_addr_ts] !=
            m_pps->TileId[m_pps->CtbAddrRsToTs[ctb_addr_rs - m_sps->ctb_width]])
      boundary_flags |= BOUNDARY_UPPER_TILE;
    if (y_ctb > 0 && tab_slice_address[ctb_addr_rs] !=
                         tab_slice_address[ctb_addr_rs - m_sps->ctb_width])
      boundary_flags |= BOUNDARY_UPPER_SLICE;
  } else {
    if (ctb_addr_in_slice <= 0) boundary_flags |= BOUNDARY_LEFT_SLICE;
    if (ctb_addr_in_slice < m_sps->ctb_width)
      boundary_flags |= BOUNDARY_UPPER_SLICE;
  }

  ctb_left_flag = ((x_ctb > 0) && (ctb_addr_in_slice > 0) &&
                   !(boundary_flags & BOUNDARY_LEFT_TILE));
  ctb_up_flag = ((y_ctb > 0) && (ctb_addr_in_slice >= m_sps->ctb_width) &&
                 !(boundary_flags & BOUNDARY_UPPER_TILE));
  ctb_up_right_flag =
      ((y_ctb > 0) && (ctb_addr_in_slice + 1 >= m_sps->ctb_width) &&
       (m_pps->TileId[ctb_addr_ts] ==
        m_pps->TileId[m_pps->CtbAddrRsToTs[ctb_addr_rs + 1 -
                                           m_sps->ctb_width]]));
  ctb_up_left_flag = ((x_ctb > 0) && (y_ctb > 0) &&
                      (ctb_addr_in_slice - 1 >= m_sps->ctb_width) &&
                      (m_pps->TileId[ctb_addr_ts] ==
                       m_pps->TileId[m_pps->CtbAddrRsToTs[ctb_addr_rs - 1 -
                                                          m_sps->ctb_width]]));
}

//6.4.1 Derivation process for z-scan order block availability
int SliceData::derivation_z_scan_order_block_availability(int xCurr, int yCurr,
                                                          int xNbY, int yNbY) {
  std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__ << std::endl;
  exit(0);
  //int minBlockAddrCurr =
  //MinTbAddrZs[xCurr >> MinTbLog2SizeY][yCurr >> MinTbLog2SizeY];

  //int minBlockAddrN;
  //if (xNbY < 0 || yNbY < 0 || xNbY >= m_sps->pic_width_in_luma_samples ||
  //yNbY >= m_sps->pic_height_in_luma_samples) {
  //minBlockAddrN = -1;
  //} else {
  //minBlockAddrN = MinTbAddrZs[xNbY >> MinTbLog2SizeY][yNbY >> MinTbLog2SizeY];
  //}

  //bool availableN = true;
  //if (minBlockAddrN < 0 || minBlockAddrN > minBlockAddrCurr) {
  //availableN = false;
  //}

  //#define MIN_TB_ADDR_ZS(x, y)                                                   \
//  s->ps.pps->min_tb_addr_zs[(y) * (s->ps.sps->tb_mask + 2) + (x)]
  //
  //  int xCurr_ctb = xCurr >> s->ps.sps->CtbLog2SizeY;
  //  int yCurr_ctb = yCurr >> s->ps.sps->CtbLog2SizeY;
  //  int xN_ctb = xN >> s->ps.sps->CtbLog2SizeY;
  //  int yN_ctb = yN >> s->ps.sps->CtbLog2SizeY;
  //  if (yN_ctb < yCurr_ctb || xN_ctb < xCurr_ctb)
  //    return 1;
  //  else {
  //    int Curr = MIN_TB_ADDR_ZS(
  //        (xCurr >> s->ps.sps->log2_min_tb_size) & s->ps.sps->tb_mask,
  //        (yCurr >> s->ps.sps->log2_min_tb_size) & s->ps.sps->tb_mask);
  //    int N = MIN_TB_ADDR_ZS(
  //        (xN >> s->ps.sps->log2_min_tb_size) & s->ps.sps->tb_mask,
  //        (yN >> s->ps.sps->log2_min_tb_size) & s->ps.sps->tb_mask);
  //    return N <= Curr;
  //  }

  return 0;
}

//6.5.2 Z-scan order array initialization process
int SliceData::Z_scan_order_array_initialization() {
  std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__ << std::endl;
  exit(0);
  int MinTbAddrZs[32][32];
  //for (int y = 0; y < (m_sps->PicHeightInCtbsY
  //<< (m_sps->CtbLog2SizeY - m_sps->MinTbLog2SizeY));
  //y++)
  //for (int x = 0; x < (m_sps->PicWidthInCtbsY
  //<< (m_sps->CtbLog2SizeY - m_sps->MinTbLog2SizeY));
  //x++) {
  //int tbX = (x << m_sps->MinTbLog2SizeY) >> m_sps->CtbLog2SizeY;
  //int tbY = (y << m_sps->MinTbLog2SizeY) >> m_sps->CtbLog2SizeY;
  //int ctbAddrRs = PicWidthInCtbsY * tbY + tbX;
  //MinTbAddrZs[x][y] =
  //CtbAddrRsToTs[ctbAddrRs]
  //<< ((m_sps->CtbLog2SizeY - m_sps->MinTbLog2SizeY) * 2);
  //int p = 0;
  //for (int i = 0, p = 0; i < (CtbLog2SizeY − MinTbLog2SizeY); i++) {
  //int m = 1 << i;
  //p += (m & x ? m * m : 0) + (m & y ? 2 * m * m : 0);
  //}
  //MinTbAddrZs[x][y] += p;
  //}
  return 0;
}

int SliceData::coding_tree_unit() {
  // CtbAddrInRs 是当前CTU在光栅顺序（Raster Scan）中的地址
  // CTU在图像中的水平、垂直像素坐标
  int32_t xCtb = (CtbAddrInRs % m_sps->PicWidthInCtbsY) << m_sps->CtbLog2SizeY;
  int32_t yCtb = (CtbAddrInRs / m_sps->PicWidthInCtbsY) << m_sps->CtbLog2SizeY;

  // 样值自适应偏移（SAO，Sample Adaptive Offset）
  if (header->slice_sao_luma_flag || header->slice_sao_chroma_flag)
    // 将CTU的像素坐标转换为CTU的索引（即CTU在图像中的位置）
    sao(xCtb >> m_sps->CtbLog2SizeY, yCtb >> m_sps->CtbLog2SizeY);

  // 递归地处理当前CTU的四叉树（QuadTree）结构
  coding_quadtree(xCtb, yCtb, m_sps->CtbLog2SizeY, 0);
  return 0;
}

int SliceData::sao(int32_t rx, int32_t ry) {
  std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__ << std::endl;
  exit(0);
  int sao_merge_left_flag = 0;
  if (rx > 0) {
    int leftCtbInSliceSeg = CtbAddrInRs > header->SliceAddrRs;
    int leftCtbInTile = (m_pps->TileId[CtbAddrInTs] ==
                         m_pps->TileId[m_pps->CtbAddrRsToTs[CtbAddrInRs - 1]]);
    if (leftCtbInSliceSeg && leftCtbInTile) {
      sao_merge_left_flag = cabac->decode_bin(SAO_MERGE_FLAG);
      std::cout << "sao_merge_left_flag:" << sao_merge_left_flag << std::endl;
    }
  }

  int sao_merge_up_flag = 0;
  if (ry > 0 && !sao_merge_left_flag) {
    int upCtbInSliceSeg =
        (CtbAddrInRs - m_sps->PicWidthInCtbsY) >= header->SliceAddrRs;
    int upCtbInTile =
        m_pps->TileId[CtbAddrInTs] ==
        m_pps->TileId[m_pps->CtbAddrRsToTs[CtbAddrInRs -
                                           m_sps->PicWidthInCtbsY]];
    if (upCtbInSliceSeg && upCtbInTile) {
      sao_merge_up_flag =
          cabac->decode_bin(elem_offset[SAO_MERGE_FLAG]); // ae(v);
      std::cout << "sao_merge_up_flag:" << sao_merge_up_flag << std::endl;
    }
  }

  int SaoTypeIdx[3][32][32] = {{{0}}};
  int sao_offset_abs[3][32][32][4] = {{{{0}}}};
  int sao_offset_sign[3][32][32][32] = {{{{0}}}};
  int sao_band_position[3][32][32] = {{{0}}};
  if (!sao_merge_up_flag && !sao_merge_left_flag)
    for (int cIdx = 0; cIdx < (m_sps->ChromaArrayType != 0 ? 3 : 1); cIdx++)
      if ((header->slice_sao_luma_flag && cIdx == 0) ||
          (header->slice_sao_chroma_flag && cIdx > 0)) {
        if (cIdx == 0) {
          int sao_type_idx_luma; // = ae(v);
          cabac->decode_sao_type_idx_luma(sao_type_idx_luma);
          if (sao_type_idx_luma) {
            SaoTypeIdx[0][rx][ry] = sao_type_idx_luma;
          } else {
            if (sao_merge_left_flag == 1) {
              SaoTypeIdx[0][rx][ry] = SaoTypeIdx[0][rx - 1][ry];
            } else if (sao_merge_up_flag == 1) {
              SaoTypeIdx[0][rx][ry] = SaoTypeIdx[0][rx][ry - 1];
            } else {
              SaoTypeIdx[0][rx][ry] = 0;
            }
          }
        } else if (cIdx == 1) {
          int sao_type_idx_chroma; // = ae(v);
        }
        if (SaoTypeIdx[cIdx][rx][ry] != 0) {
          for (int i = 0; i < 4; i++)
            cabac->decode_sao_offset_abs(sao_offset_abs[cIdx][rx][ry][i]);
          if (SaoTypeIdx[cIdx][rx][ry] == 1) {
            for (int i = 0; i < 4; i++) {
              if (sao_offset_abs[cIdx][rx][ry][i] != 0) {
                cabac->decode_sao_offset_sign(sao_offset_sign[cIdx][rx][ry][i]);
                if (sao_offset_sign[cIdx][rx][ry][i])
                  sao_offset_abs[cIdx][rx][ry][i] =
                      -sao_offset_abs[cIdx][rx][ry][i];
              }
            }
            cabac->decode_sao_band_position(sao_band_position[cIdx][rx][ry]);
          } else {
            if (cIdx == 0) int sao_eo_class_luma = 0;   // = ae(v);
            if (cIdx == 1) int sao_eo_class_chroma = 0; // = ae(v);
          }
        }
      }
  return 0;
}

void SliceData::set_ct_depth(SPS *sps, int x0, int y0, int log2_cb_size,
                             int ct_depth) {
  int length = (1 << log2_cb_size) >> sps->log2_min_luma_coding_block_size;
  int x_cb = x0 >> sps->log2_min_luma_coding_block_size;
  int y_cb = y0 >> sps->log2_min_luma_coding_block_size;

  for (int y = 0; y < length; y++)
    memset(&tab_ct_depth[(y_cb + y) * sps->min_cb_width + x_cb], ct_depth,
           length);
}

int SliceData::coding_quadtree(int x0, int y0, int log2CbSize, int cqtDepth) {
  int32_t pic_width_in_luma_samples = m_sps->pic_width_in_luma_samples;
  int32_t pic_height_in_luma_samples = m_sps->pic_height_in_luma_samples;
  int32_t MinCbLog2SizeY = m_sps->MinCbLog2SizeY;

  this->ct_depth = cqtDepth;
  int split_cu_flag[32][32] = {{0}};
  IsCuQpDeltaCoded = 0, CuQpDeltaVal = 0, IsCuChromaQpOffsetCoded = 0;

  int Log2MinCuChromaQpOffsetSize =
      m_sps->CtbLog2SizeY - m_pps->diff_cu_chroma_qp_offset_depth;

  if (x0 + (1 << log2CbSize) <= pic_width_in_luma_samples &&
      y0 + (1 << log2CbSize) <= pic_height_in_luma_samples &&
      log2CbSize > MinCbLog2SizeY) {
    cabac->decode_split_cu_flag(split_cu_flag[x0][y0], *m_sps, tab_ct_depth,
                                ctb_left_flag, ctb_up_flag, ct_depth, x0, y0);
    std::cout << "[if]split_cu_flag:" << split_cu_flag[x0][y0]
              << ",cqtDepth:" << cqtDepth << std::endl;
  } else {
    split_cu_flag[x0][y0] =
        (log2CbSize > m_sps->log2_min_luma_coding_block_size);
    std::cout << "[else]split_cu_flag:" << split_cu_flag[x0][y0]
              << ",cqtDepth:" << cqtDepth << std::endl;
  }

  if (m_pps->cu_qp_delta_enabled_flag &&
      log2CbSize >= m_pps->Log2MinCuQpDeltaSize)
    IsCuQpDeltaCoded = 0, CuQpDeltaVal = 0;

  if (header->cu_chroma_qp_offset_enabled_flag &&
      log2CbSize >= Log2MinCuChromaQpOffsetSize)
    IsCuChromaQpOffsetCoded = 0;

  if (split_cu_flag[x0][y0]) {
    int x1 = x0 + (1 << (log2CbSize - 1));
    int y1 = y0 + (1 << (log2CbSize - 1));
    coding_quadtree(x0, y0, log2CbSize - 1, cqtDepth + 1);
    if (x1 < pic_width_in_luma_samples)
      coding_quadtree(x1, y0, log2CbSize - 1, cqtDepth + 1);
    if (y1 < pic_height_in_luma_samples)
      coding_quadtree(x0, y1, log2CbSize - 1, cqtDepth + 1);
    if (x1 < pic_width_in_luma_samples && y1 < pic_height_in_luma_samples)
      coding_quadtree(x1, y1, log2CbSize - 1, cqtDepth + 1);
  } else
    coding_unit(x0, y0, log2CbSize);
  return 0;
}

int SliceData::coding_unit(int x0, int y0, int log2CbSize) {
  cu_transquant_bypass_flag = false;
  //CuPredMode[32][32] = {{0}};
  int palette_mode_flag[32][32] = {{0}};
  MaxTbLog2SizeY = m_sps->log2_min_luma_transform_block_size +
                   m_sps->log2_diff_max_min_luma_transform_block_size;
  int part_mode = PART_2Nx2N;
  int &PartMode = part_mode;
  IntraSplitFlag = 0;

  int x_cb = x0 >> m_sps->log2_min_luma_coding_block_size;
  int y_cb = y0 >> m_sps->log2_min_luma_coding_block_size;

  CuPredMode[x0][y0] = MODE_INTRA;
  cu_skip_flag[x0][y0] = 0;

  static int yangjing = 0;
  yangjing++;
  if (yangjing == 2) {
    std::cout << "yangjing = " << yangjing << std::endl;
  }

  if (m_pps->transquant_bypass_enabled_flag) {
    cu_transquant_bypass_flag = cabac->decode_bin(IHEVC_CAB_CU_TQ_BYPASS_FLAG);
    std::cout << "cu_transquant_bypass_flag:" << cu_transquant_bypass_flag
              << std::endl;
  }
  if (header->slice_type != HEVC_SLICE_I) {
    cu_skip_flag[x0][y0] =
        cabac->decode_cu_skip_flag(x0, y0, x_cb, y_cb, ctb_left_flag,
                                   ctb_up_left_flag, cu_skip_flag); //ae(v);
    std::cout << "cu_skip_flag:" << cu_skip_flag[x0][y0] << std::endl;
    CuPredMode[x0][y0] = cu_skip_flag[x0][y0] ? MODE_SKIP : MODE_INTER;
  } else {
    /* TODO YangJing ??? <25-01-26 15:24:15> */
  }

  int nCbS = (1 << log2CbSize);
  if (cu_skip_flag[x0][y0])
    prediction_unit(x0, y0, nCbS, nCbS);
  else {
    if (header->slice_type != HEVC_SLICE_I) {
      int pred_mode_flag = 0; //ae(v);
      std::cout << "pred_mode_flag:" << pred_mode_flag << std::endl;
      CuPredMode[x0][y0] = pred_mode_flag;
    }
    if (m_sps->palette_mode_enabled_flag && CuPredMode[x0][y0] == MODE_INTRA &&
        log2CbSize <= MaxTbLog2SizeY) {
      palette_mode_flag[x0][y0] = 0; //ae(v);
      std::cout << "palette_mode_flag:" << palette_mode_flag[x0][y0]
                << std::endl;
    }
    if (palette_mode_flag[x0][y0])
      palette_coding(x0, y0, nCbS);
    else {
      int pcm_flag[32][32] = {0};
      if (CuPredMode[x0][y0] != MODE_INTRA ||
          log2CbSize == m_sps->MinCbLog2SizeY) {
        part_mode = cabac->ff_hevc_part_mode_decode(
            log2CbSize, CuPredMode[x0][y0]); //ae(v);
        std::cout << "part_mode:" << part_mode << std::endl;
        IntraSplitFlag =
            part_mode == PART_NxN && CuPredMode[x0][y0] == MODE_INTRA;

        if (yangjing == 2) {
          exit(0);
        }
      }
      if (CuPredMode[x0][y0] == MODE_INTRA) {
        if (PartMode == PART_2Nx2N && m_sps->pcm_enabled_flag &&
            log2CbSize >= m_sps->log2_min_pcm_luma_coding_block_size &&
            log2CbSize <= m_sps->log2_max_pcm_luma_coding_block_size) {
          pcm_flag[x0][y0] = 0; //ae(v);
          std::cout << "pcm_flag:" << pcm_flag[x0][y0] << std::endl;
        }
        if (pcm_flag[x0][y0]) {
          while (!bs->byte_aligned()) {
            int pcm_alignment_zero_bit = 0; //f(1);
            std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__
                      << std::endl;
          }
          pcm_sample(x0, y0, log2CbSize);
        } else {
          // NOTE: 帧内预测 ffmepg -> intra_prediction_unit(s, x0, y0, log2CbSize);
          int i, j;
          int prev_intra_luma_pred_flag[32][32] = {0};
          int mpm_idx[32][32] = {0};
          rem_intra_luma_pred_mode[32][32] = {0};
          intra_chroma_pred_mode[32][32] = {0};
          int pbOffset = (PartMode == PART_NxN) ? (nCbS / 2) : nCbS;
          for (j = 0; j < nCbS; j = j + pbOffset)
            for (i = 0; i < nCbS; i = i + pbOffset) {
              prev_intra_luma_pred_flag[x0 + i][y0 + j] = cabac->decode_bin(
                  elem_offset[PREV_INTRA_LUMA_PRED_FLAG]); // ae(v);
              std::cout << "prev_intra_luma_pred_flag:"
                        << prev_intra_luma_pred_flag[x0 + i][y0 + j]
                        << std::endl;
            }
          for (j = 0; j < nCbS; j = j + pbOffset)
            for (i = 0; i < nCbS; i = i + pbOffset)
              if (prev_intra_luma_pred_flag[x0 + i][y0 + j]) {
                mpm_idx[x0 + i][y0 + j] =
                    cabac->ff_hevc_mpm_idx_decode(); // ae(v);
                std::cout << "mpm_idx:" << mpm_idx[x0 + i][y0 + j] << std::endl;
              } else {
                rem_intra_luma_pred_mode[x0 + i][y0 + j] =
                    cabac->ff_hevc_rem_intra_luma_pred_mode_decode(); // ae(v);
                std::cout << "rem_intra_luma_pred_mode:"
                          << rem_intra_luma_pred_mode[x0 + i][y0 + j]
                          << std::endl;
              }
          if (m_sps->ChromaArrayType == 3) {
            for (j = 0; j < nCbS; j = j + pbOffset)
              for (i = 0; i < nCbS; i = i + pbOffset) {
                intra_chroma_pred_mode[x0 + 1][y0 + j] =
                    cabac->ff_hevc_intra_chroma_pred_mode_decode(); //ae(v);
                std::cout << "intra_chroma_pred_mode:"
                          << intra_chroma_pred_mode[x0 + 1][y0 + j]
                          << std::endl;
              }
          } else if (m_sps->ChromaArrayType != 0) {
            intra_chroma_pred_mode[x0][y0] =
                cabac->ff_hevc_intra_chroma_pred_mode_decode(); //ae(v);
            if (m_sps->ChromaArrayType == 2) {
              std::cout << "intra_chroma_pred_mode:"
                        << intra_chroma_pred_mode[x0][y0] << std::endl;
            }
          }
        }
      } else {
        if (PartMode == PART_2Nx2N)
          prediction_unit(x0, y0, nCbS, nCbS);
        else if (PartMode == PART_2NxN) {
          prediction_unit(x0, y0, nCbS, nCbS / 2);
          prediction_unit(x0, y0 + (nCbS / 2), nCbS, nCbS / 2);
        } else if (PartMode == PART_Nx2N) {
          prediction_unit(x0, y0, nCbS / 2, nCbS);
          prediction_unit(x0 + (nCbS / 2), y0, nCbS / 2, nCbS);
        } else if (PartMode == PART_2NxnU) {
          prediction_unit(x0, y0, nCbS, nCbS / 4);
          prediction_unit(x0, y0 + (nCbS / 4), nCbS, nCbS * 3 / 4);
        } else if (PartMode == PART_2NxnD) {
          prediction_unit(x0, y0, nCbS, nCbS * 3 / 4);
          prediction_unit(x0, y0 + (nCbS * 3 / 4), nCbS, nCbS / 4);
        } else if (PartMode == PART_nLx2N) {
          prediction_unit(x0, y0, nCbS / 4, nCbS);
          prediction_unit(x0 + (nCbS / 4), y0, nCbS * 3 / 4, nCbS);
        } else if (PartMode == PART_nRx2N) {
          prediction_unit(x0, y0, nCbS * 3 / 4, nCbS);
          prediction_unit(x0 + (nCbS * 3 / 4), y0, nCbS / 4, nCbS);
        } else { /* PART_NxN */
          prediction_unit(x0, y0, nCbS / 2, nCbS / 2);
          prediction_unit(x0 + (nCbS / 2), y0, nCbS / 2, nCbS / 2);
          prediction_unit(x0, y0 + (nCbS / 2), nCbS / 2, nCbS / 2);
          prediction_unit(x0 + (nCbS / 2), y0 + (nCbS / 2), nCbS / 2, nCbS / 2);
        }
      }
      if (!pcm_flag[x0][y0]) {
        int rqt_root_cbf = 1;
        if (CuPredMode[x0][y0] != MODE_INTRA &&
            !(PartMode == PART_2Nx2N && merge_flag[x0][y0])) {
          rqt_root_cbf =
              cabac->decode_bin(elem_offset[NO_RESIDUAL_DATA_FLAG]); //ae(v);
          std::cout << "rqt_root_cbf:" << rqt_root_cbf << std::endl;
        }
        if (rqt_root_cbf) {
          MaxTrafoDepth = (CuPredMode[x0][y0] == MODE_INTRA
                               ? (m_sps->max_transform_hierarchy_depth_intra +
                                  IntraSplitFlag)
                               : m_sps->max_transform_hierarchy_depth_inter);
          transform_tree(x0, y0, x0, y0, log2CbSize, 0, 0);
        }
      }
    }
  }

  set_ct_depth(m_sps, x0, y0, log2CbSize, ct_depth);
  return 0;
}

int SliceData::prediction_unit(int x0, int y0, int nPbW, int nPbH) {
  int inter_pred_idc[32][32] = {{0}};
  int mvp_l0_flag[32][32] = {{0}};
  int mvp_l1_flag[32][32] = {{0}};
  int ref_idx_l0[32][32] = {{0}};
  int ref_idx_l1[32][32] = {{0}};
  int MvdL1[32][32][2] = {{0}};

  if (cu_skip_flag[x0][y0]) {
    if (header->MaxNumMergeCand > 1) {
      merge_idx[x0][y0] =
          cabac->ff_hevc_merge_idx_decode(header->MaxNumMergeCand); //ae(v);
      std::cout << "merge_idx:" << merge_idx[x0][y0] << std::endl;
    } else {
      merge_idx[x0][y0] = 0;
    }
  } else { /* MODE_INTER */
    merge_flag[x0][y0] = cabac->decode_bin(elem_offset[MERGE_FLAG]); //ae(v);
    std::cout << "merge_flag[x0][y0]:" << merge_flag[x0][y0] << std::endl;
    if (merge_flag[x0][y0]) {
      if (header->MaxNumMergeCand > 1) {
        merge_idx[x0][y0] =
            cabac->ff_hevc_merge_idx_decode(header->MaxNumMergeCand); //ae(v);
        std::cout << "merge_idx[x0][y0]:" << merge_idx[x0][y0] << std::endl;
      }
    } else {
      if (header->slice_type == HEVC_SLICE_B) {
        inter_pred_idc[x0][y0] =
            cabac->ff_hevc_inter_pred_idc_decode(nPbW, nPbH, ct_depth); //ae(v);
        std::cout << "inter_pred_idc[x0][y0]:" << inter_pred_idc[x0][y0]
                  << std::endl;
      }
      if (inter_pred_idc[x0][y0] != PRED_L1) {
        if (header->num_ref_idx_l0_active_minus1 > 0) {
          /* TODO YangJing 这里传入num_ref_idx_l0_active_minus1 + 1? <25-01-01 21:01:54> */
          ref_idx_l0[x0][y0] = cabac->ff_hevc_ref_idx_lx_decode(
              header->num_ref_idx_l0_active_minus1); //ae(v);
          std::cout << "ref_idx_l0[x0][y0]:" << ref_idx_l0[x0][y0] << std::endl;
        }
        mvd_coding(x0, y0, 0);
        mvp_l0_flag[x0][y0] =
            cabac->decode_bin(elem_offset[MVP_LX_FLAG]); //ae(v);
        std::cout << "mvp_l0_flag[x0][y0]:" << mvp_l0_flag[x0][y0] << std::endl;
      }
      if (inter_pred_idc[x0][y0] != PRED_L0) {
        if (header->num_ref_idx_l1_active_minus1 > 0) {
          /* TODO YangJing 这里传入num_ref_idx_l0_active_minus1 + 1? <25-01-01 21:01:54> */
          ref_idx_l1[x0][y0] = cabac->ff_hevc_ref_idx_lx_decode(
              header->num_ref_idx_l1_active_minus1); //ae(v);
          std::cout << "ref_idx_l1[x0][y0]:" << ref_idx_l1[x0][y0] << std::endl;
        }
        if (header->mvd_l1_zero_flag && inter_pred_idc[x0][y0] == PRED_BI) {
          MvdL1[x0][y0][0] = 0, MvdL1[x0][y0][1] = 0;
        } else
          mvd_coding(x0, y0, 1);
        mvp_l1_flag[x0][y0] =
            cabac->decode_bin(elem_offset[MVP_LX_FLAG]); //ae(v);
        std::cout << "mvp_l1_flag[x0][y0]:" << mvp_l1_flag[x0][y0] << std::endl;
      }
    }
  }
  return 0;
}

int SliceData::pcm_sample(int x0, int y0, int log2CbSize) {
  std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__ << std::endl;
  exit(0);
  int i;
  uint8_t *pcm_sample_luma = new uint8_t[1 << (log2CbSize << 1)];
  uint8_t *pcm_sample_chroma = new uint8_t[1 << (log2CbSize << 1)];
  for (i = 0; i < 1 << (log2CbSize << 1); i++)
    pcm_sample_luma[i] = 0; //u(v); TODO  <24-12-15 18:09:51, YangJing>
  if (m_sps->ChromaArrayType != 0)
    for (i = 0; i < ((2 << (log2CbSize << 1)) /
                     (m_sps->SubWidthC * m_sps->SubHeightC));
         i++)
      pcm_sample_chroma[i] = 0; //u(v); TODO  <24-12-15 18:10:24, YangJing>

  return 0;
}

int SliceData::transform_tree(int x0, int y0, int xBase, int yBase,
                              int log2TrafoSize, int trafoDepth, int blkIdx) {
  int MinTbLog2SizeY = m_sps->log2_min_luma_transform_block_size;

  //初始化后续使用的字段
  if (IntraSplitFlag) {
    if (trafoDepth == 1) {
      m_tu.intra_pred_mode = m_pu.intra_pred_mode[blkIdx];
      if (m_sps->chroma_format_idc == 3) {
        m_tu.intra_pred_mode_c = m_pu.intra_pred_mode_c[blkIdx];
        m_tu.chroma_mode_c = m_pu.intra_chroma_pred_mode[blkIdx];
      } else {
        m_tu.intra_pred_mode_c = m_pu.intra_pred_mode_c[0];
        m_tu.chroma_mode_c = m_pu.intra_chroma_pred_mode[0];
      }
    }
  } else {
    m_tu.intra_pred_mode = m_pu.intra_pred_mode[0];
    m_tu.intra_pred_mode_c = m_pu.intra_pred_mode_c[0];
    m_tu.chroma_mode_c = m_pu.intra_chroma_pred_mode[0];
  }
  //}

  if (log2TrafoSize <= MaxTbLog2SizeY && log2TrafoSize > MinTbLog2SizeY &&
      trafoDepth < MaxTrafoDepth && !(IntraSplitFlag && (trafoDepth == 0))) {
    split_transform_flag[x0][y0][trafoDepth] = cabac->decode_bin(
        elem_offset[SPLIT_TRANSFORM_FLAG] + 5 - log2TrafoSize); // ae(v);
    std::cout << "split_transform_flag[x0][y0][trafoDepth]:"
              << split_transform_flag[x0][y0][trafoDepth] << std::endl;
  }
  if ((log2TrafoSize > 2 && m_sps->ChromaArrayType != 0) ||
      m_sps->ChromaArrayType == 3) {
    if (trafoDepth == 0 || cbf_cb[xBase][yBase][trafoDepth - 1]) {
      cbf_cb[x0][y0][trafoDepth] =
          cabac->decode_bin(elem_offset[CBF_CB_CR] + trafoDepth); // ae(v);
      if (m_sps->ChromaArrayType == 2 &&
          (!split_transform_flag[x0][y0][trafoDepth] || log2TrafoSize == 3))
        cbf_cb[x0][y0 + (1 << (log2TrafoSize - 1))][trafoDepth] =
            cabac->decode_bin(elem_offset[CBF_CB_CR] + trafoDepth); // ae(v);
    }
    if (trafoDepth == 0 || cbf_cr[xBase][yBase][trafoDepth - 1]) {
      cbf_cr[x0][y0][trafoDepth] =
          cabac->decode_bin(elem_offset[CBF_CB_CR] + trafoDepth); // ae(v);
      if (m_sps->ChromaArrayType == 2 &&
          (!split_transform_flag[x0][y0][trafoDepth] || log2TrafoSize == 3))
        cbf_cr[x0][y0 + (1 << (log2TrafoSize - 1))][trafoDepth] =
            cabac->decode_bin(elem_offset[CBF_CB_CR] + trafoDepth); // ae(v);
    }
  }
  if (split_transform_flag[x0][y0][trafoDepth]) {
    int x1 = x0 + (1 << (log2TrafoSize - 1));
    int y1 = y0 + (1 << (log2TrafoSize - 1));
    transform_tree(x0, y0, x0, y0, log2TrafoSize - 1, trafoDepth + 1, 0);
    transform_tree(x1, y0, x0, y0, log2TrafoSize - 1, trafoDepth + 1, 1);
    transform_tree(x0, y1, x0, y0, log2TrafoSize - 1, trafoDepth + 1, 2);
    transform_tree(x1, y1, x0, y0, log2TrafoSize - 1, trafoDepth + 1, 3);
  } else {
    if (CuPredMode[x0][y0] == MODE_INTRA || trafoDepth != 0 ||
        cbf_cb[x0][y0][trafoDepth] || cbf_cr[x0][y0][trafoDepth] ||
        (m_sps->ChromaArrayType == 2 &&
         (cbf_cb[x0][y0 + (1 << (log2TrafoSize - 1))][trafoDepth] ||
          cbf_cr[x0][y0 + (1 << (log2TrafoSize - 1))][trafoDepth])))
      cbf_luma[x0][y0][trafoDepth] =
          cabac->decode_bin(elem_offset[CBF_LUMA] + !trafoDepth); //ae(v);
    transform_unit(x0, y0, xBase, yBase, log2TrafoSize, trafoDepth, blkIdx);
  }
  return 0;
}

int SliceData::mvd_coding(int x0, int y0, int refList) {
  int abs_mvd_greater0_flag[2] = {0};
  abs_mvd_greater0_flag[0] =
      cabac->decode_bin(elem_offset[ABS_MVD_GREATER0_FLAG]); //ae(v);
  abs_mvd_greater0_flag[1] =
      cabac->decode_bin(elem_offset[ABS_MVD_GREATER0_FLAG]); //ae(v);

  std::cout << "abs_mvd_greater0_flag[0]:" << abs_mvd_greater0_flag[0]
            << std::endl;
  std::cout << "abs_mvd_greater0_flag[1]:" << abs_mvd_greater0_flag[1]
            << std::endl;

  int abs_mvd_greater1_flag[2] = {0};
  if (abs_mvd_greater0_flag[0])
    abs_mvd_greater1_flag[0] =
        cabac->decode_bin(elem_offset[ABS_MVD_GREATER1_FLAG]); // ae(v);
  if (abs_mvd_greater0_flag[1])
    abs_mvd_greater1_flag[1] =
        cabac->decode_bin(elem_offset[ABS_MVD_GREATER1_FLAG]); //ae(v);

  std::cout << "abs_mvd_greater1_flag[0]:" << abs_mvd_greater1_flag[0]
            << std::endl;
  std::cout << "abs_mvd_greater1_flag[1]:" << abs_mvd_greater1_flag[1]
            << std::endl;

  int abs_mvd_minus2[2] = {0};
  int mvd_sign_flag[2] = {0};
  if (abs_mvd_greater0_flag[0]) {
    if (abs_mvd_greater1_flag[0]) {
      abs_mvd_minus2[0] = cabac->mvd_decode(); //ae(v);
      std::cout << "abs_mvd_minus2[0]:" << abs_mvd_minus2[0] << std::endl;
    }
    mvd_sign_flag[0] = cabac->mvd_sign_flag_decode(); //ae(v);
    std::cout << "mvd_sign_flag[0]:" << mvd_sign_flag[0] << std::endl;
  }

  if (abs_mvd_greater0_flag[1]) {
    if (abs_mvd_greater1_flag[1]) {
      abs_mvd_minus2[1] = cabac->mvd_decode(); //ae(v);
      std::cout << "abs_mvd_minus2[1]:" << abs_mvd_minus2[1] << std::endl;
    }
    mvd_sign_flag[1] = cabac->mvd_sign_flag_decode(); //ae(v);
    std::cout << "mvd_sign_flag[1]:" << mvd_sign_flag[1] << std::endl;
  }

  return 0;
}

int SliceData::transform_unit(int x0, int y0, int xBase, int yBase,
                              int log2TrafoSize, int trafoDepth, int blkIdx) {
  const int &ChromaArrayType = m_sps->ChromaArrayType;
  const int &MinCbLog2SizeY = m_sps->MinCbLog2SizeY;
  int tIdx = 0;
  int log2TrafoSizeC = MAX(2, log2TrafoSize - (ChromaArrayType == 3 ? 0 : 1));
  int cbfDepthC =
      trafoDepth - (ChromaArrayType != 3 && log2TrafoSize == 2 ? 1 : 0);
  int xC = (ChromaArrayType != 3 && log2TrafoSize == 2) ? xBase : x0;
  int yC = (ChromaArrayType != 3 && log2TrafoSize == 2) ? yBase : y0;
  int cbfLuma = cbf_luma[x0][y0][trafoDepth];
  int cbfChroma = cbf_cb[xC][yC][cbfDepthC] || cbf_cr[xC][yC][cbfDepthC] ||
                  (ChromaArrayType == 2 &&
                   (cbf_cb[xC][yC + (1 << log2TrafoSizeC)][cbfDepthC] ||
                    cbf_cr[xC][yC + (1 << log2TrafoSizeC)][cbfDepthC]));
  /* TODO YangJing 这里的PartMode是乱写的，后面一定要确定 <25-01-01 22:01:36> */
  int PartMode = 1;

  if (cbfLuma || cbfChroma) {
    int xP = (x0 >> MinCbLog2SizeY) << MinCbLog2SizeY;
    int yP = (y0 >> MinCbLog2SizeY) << MinCbLog2SizeY;
    int nCbS = 1 << MinCbLog2SizeY;
    if (m_pps->residual_adaptive_colour_transform_enabled_flag &&
        (CuPredMode[x0][y0] == MODE_INTER ||
         (PartMode == PART_2Nx2N && intra_chroma_pred_mode[x0][y0] == 4) ||
         (intra_chroma_pred_mode[xP][yP] == 4 &&
          intra_chroma_pred_mode[xP + nCbS / 2][yP] == 4 &&
          intra_chroma_pred_mode[xP][yP + nCbS / 2] == 4 &&
          intra_chroma_pred_mode[xP + nCbS / 2][yP + nCbS / 2] == 4))) {
      /* TODO YangJing  <25-01-01 21:58:43> */
      //tu_residual_act_flag[x0][y0] = ae(v);
      std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__ << std::endl;
      exit(0);
    }

    int scan_idx = SCAN_DIAG;
    int scan_idx_c = SCAN_DIAG;

    delta_qp();
    if (cbfChroma && !cu_transquant_bypass_flag) {
      chroma_qp_offset();
    }

#if 0
    // For ffmpeg:
    if (CuPredMode[x0][y0] == MODE_INTRA && log2_trafo_size < 4) {
      if (lc->tu.intra_pred_mode >= 6 && lc->tu.intra_pred_mode <= 14) {
        scan_idx = SCAN_VERT;
      } else if (lc->tu.intra_pred_mode >= 22 && lc->tu.intra_pred_mode <= 30) {
        scan_idx = SCAN_HORIZ;
      }

      if (lc->tu.intra_pred_mode_c >= 6 && lc->tu.intra_pred_mode_c <= 14) {
        scan_idx_c = SCAN_VERT;
      } else if (lc->tu.intra_pred_mode_c >= 22 &&
                 lc->tu.intra_pred_mode_c <= 30) {
        scan_idx_c = SCAN_HORIZ;
      }
    }

    lc->tu.cross_pf = 0;
#endif

    if (cbfLuma)
      residual_coding(x0, y0, log2TrafoSize, (Cabac::ScanType)scan_idx, 0);
    if (log2TrafoSize > 2 || ChromaArrayType == 3) {
      if (m_pps->cross_component_prediction_enabled_flag && cbfLuma &&
          (CuPredMode[x0][y0] == MODE_INTER ||
           intra_chroma_pred_mode[x0][y0] == 4))
        cross_comp_pred(x0, y0, 0);
      for (tIdx = 0; tIdx < (ChromaArrayType == 2 ? 2 : 1); tIdx++)
        if (cbf_cb[x0][y0 + (tIdx << log2TrafoSizeC)][trafoDepth])
          residual_coding(x0, y0 + (tIdx << log2TrafoSizeC), log2TrafoSizeC,
                          (Cabac::ScanType)scan_idx, 1);
      if (m_pps->cross_component_prediction_enabled_flag && cbfLuma &&
          (CuPredMode[x0][y0] == MODE_INTER ||
           intra_chroma_pred_mode[x0][y0] == 4))
        cross_comp_pred(x0, y0, 1);
      for (tIdx = 0; tIdx < (ChromaArrayType == 2 ? 2 : 1); tIdx++)
        if (cbf_cr[x0][y0 + (tIdx << log2TrafoSizeC)][trafoDepth])
          residual_coding(x0, y0 + (tIdx << log2TrafoSizeC), log2TrafoSizeC,
                          (Cabac::ScanType)scan_idx, 2);
    } else if (blkIdx == 3) {
      for (tIdx = 0; tIdx < (ChromaArrayType == 2 ? 2 : 1); tIdx++)
        if (cbf_cb[xBase][yBase + (tIdx << log2TrafoSizeC)][trafoDepth - 1])
          residual_coding(xBase, yBase + (tIdx << log2TrafoSizeC),
                          log2TrafoSize, (Cabac::ScanType)scan_idx, 1);
      for (tIdx = 0; tIdx < (ChromaArrayType == 2 ? 2 : 1); tIdx++)
        if (cbf_cr[xBase][yBase + (tIdx << log2TrafoSizeC)][trafoDepth - 1])
          residual_coding(xBase, yBase + (tIdx << log2TrafoSizeC),
                          log2TrafoSize, (Cabac::ScanType)scan_idx, 2);
    }
  }
  return 0;
}

int SliceData::residual_coding(int x0, int y0, int log2TrafoSize,
                               Cabac::ScanType scanIdx, int cIdx) {
  uint8_t transform_skip_flag[32][32][32] = {0};
  uint8_t explicit_rdpcm_flag[32][32][32] = {0};
  uint8_t explicit_rdpcm_dir_flag[32][32][32] = {0};
  int xS, yS, xC, yC;

  if (m_pps->transform_skip_enabled_flag && !cu_transquant_bypass_flag &&
      (log2TrafoSize <= m_pps->Log2MaxTransformSkipSize))
    transform_skip_flag[x0][y0][cIdx] =
        cabac->decode_bin(elem_offset[TRANSFORM_SKIP_FLAG] + !!cIdx);

  if (CuPredMode[x0][y0] == MODE_INTER && m_sps->explicit_rdpcm_enabled_flag &&
      (transform_skip_flag[x0][y0][cIdx] || cu_transquant_bypass_flag)) {
    explicit_rdpcm_flag[x0][y0][cIdx] =
        cabac->decode_bin(elem_offset[EXPLICIT_RDPCM_FLAG] + !!cIdx); //ae(v);
    std::cout << "explicit_rdpcm_flag[x0][y0][cIdx]:"
              << explicit_rdpcm_flag[x0][y0][cIdx] << std::endl;
    if (explicit_rdpcm_flag[x0][y0][cIdx]) {
      explicit_rdpcm_dir_flag[x0][y0][cIdx] = cabac->decode_bin(
          elem_offset[EXPLICIT_RDPCM_DIR_FLAG] + !!cIdx); //ae(v);
      std::cout << "explicit_rdpcm_dir_flag[x0][y0][cIdx]:"
                << explicit_rdpcm_dir_flag[x0][y0][cIdx] << std::endl;
    }
  }

  int last_sig_coeff_x_prefix; //ae(v);
  int last_sig_coeff_y_prefix; //ae(v);
  int last_sig_coeff_x_suffix, last_sig_coeff_y_suffix;
  int LastSignificantCoeffX, LastSignificantCoeffY;
  cabac->last_significant_coeff_xy_prefix_decode(
      cIdx, log2TrafoSize, &last_sig_coeff_x_prefix, &last_sig_coeff_y_prefix);

  if (last_sig_coeff_x_prefix > 3) {
    int suffix =
        cabac->last_significant_coeff_suffix_decode(last_sig_coeff_x_prefix);
    LastSignificantCoeffX = last_sig_coeff_x_suffix =
        (1 << ((last_sig_coeff_x_prefix >> 1) - 1)) *
            (2 + (last_sig_coeff_x_prefix & 1)) +
        suffix; //ae(v);
  } else {
    LastSignificantCoeffX = last_sig_coeff_x_prefix;
  }
  if (last_sig_coeff_y_prefix > 3) {
    int suffix =
        cabac->last_significant_coeff_suffix_decode(last_sig_coeff_y_prefix);
    LastSignificantCoeffY = last_sig_coeff_y_suffix =
        (1 << ((last_sig_coeff_y_prefix >> 1) - 1)) *
            (2 + (last_sig_coeff_y_prefix & 1)) +
        suffix;
  } else {
    LastSignificantCoeffY = last_sig_coeff_y_prefix;
  }
  std::cout << "LastSignificantCoeffX:" << LastSignificantCoeffX << std::endl;
  std::cout << "LastSignificantCoeffY:" << LastSignificantCoeffY << std::endl;

#define FFSWAP(type, a, b)                                                     \
  do {                                                                         \
    type SWAP_tmp = b;                                                         \
    b = a;                                                                     \
    a = SWAP_tmp;                                                              \
  } while (0)
  //When scanIdx is equal to 2, the coordinates are swapped as follows:
  if (scanIdx == Cabac::SCAN_VERT) {
    FFSWAP(int, LastSignificantCoeffX, LastSignificantCoeffY);
  }

  /* TODO YangJing  <25-01-26 10:03:32> */
  //ScanOrder[ log2BlockSize ][ scanIdx ][ sPos ][ sComp ]
  uint8_t ScanOrder[6][4][16][2] = {0};

  //对于从 0 到 3（含）的 log2BlockSize，使用 1 << log2BlockSize 作为输入调用第 6.5.3 节中指定的右上对角线扫描顺序数组初始化过程，并将输出分配给 ScanOrder[ log2BlockSize ][ 0 ] 。
  //=============================================不确定是否对
  int log2BlockSize = 0;
  for (int i = 0; i < 6; ++i) {
    log2BlockSize = i;
    if (log2BlockSize >= 0 && log2BlockSize <= 3) {
      Up_right_diagonal_scan_order_array_initialization_process(
          1 << log2BlockSize, ScanOrder[log2BlockSize][0]);
      Horizontal_scan_order_array_initialization_process(
          1 << log2BlockSize, ScanOrder[log2BlockSize][1]);
      Vertical_scan_order_array_initialization_process(
          1 << log2BlockSize, ScanOrder[log2BlockSize][2]);
    } else {
      Traverse_scan_order_array_initialization_process(
          1 << log2BlockSize, ScanOrder[log2BlockSize][3]);
    }
  }
  //=============================================

  // scanIdx等于0指定右上对角扫描顺序，scanIdx等于1指定水平扫描顺序，scanIdx等于2指定垂直扫描顺序，scanIdx等于3指定横向扫描顺序
  int lastScanPos = 16; //lastScanPos 表示在 4x4 子块内的扫描位置
  //最后一个子块的索引
  int lastSubBlock =
      (1 << (log2TrafoSize - 2)) * (1 << (log2TrafoSize - 2)) - 1;
  do {
    if (lastScanPos == 0) {
      lastScanPos = 16;
      lastSubBlock--;
    }
    lastScanPos--;
    xS = ScanOrder[log2TrafoSize - 2][scanIdx][lastSubBlock][0];
    yS = ScanOrder[log2TrafoSize - 2][scanIdx][lastSubBlock][1];
    xC = (xS << 2) + ScanOrder[2][scanIdx][lastScanPos][0];
    yC = (yS << 2) + ScanOrder[2][scanIdx][lastScanPos][1];
  } while ((xC != LastSignificantCoeffX) || (yC != LastSignificantCoeffY));

  int i, n;
  uint8_t coded_sub_block_flag[32][32] = {0};
  uint8_t sig_coeff_flag[32][32] = {0};
  uint8_t coeff_abs_level_greater1_flag[15] = {0};
  for (i = lastSubBlock; i >= 0; i--) {
    xS = ScanOrder[log2TrafoSize - 2][scanIdx][i][0];
    yS = ScanOrder[log2TrafoSize - 2][scanIdx][i][1];
    int escapeDataPresent = 0;
    int inferSbDcSigCoeffFlag = 0;
    if ((i < lastSubBlock) && (i > 0)) {
      /* TODO YangJing coded_sub_block_flag用sig_coeff_flag的解码？ <25-01-26 14:30:02> */
      //coded_sub_block_flag[xS][yS] = cabac->significant_coeff_group_flag_decode(cIdx,ctx_cg); //ae(v);
      inferSbDcSigCoeffFlag = 1;
      std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__ << std::endl;
      exit(0);
    }
    for (n = (i == lastSubBlock) ? lastScanPos - 1 : 15; n >= 0; n--) {
      xC = (xS << 2) + ScanOrder[2][scanIdx][n][0];
      yC = (yS << 2) + ScanOrder[2][scanIdx][n][1];
      if (coded_sub_block_flag[xS][yS] && (n > 0 || !inferSbDcSigCoeffFlag)) {
        std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__
                  << std::endl;
        exit(0);
        /* TODO YangJing 问题在这里，应该要进入的，但是这里没有进入 <25-01-26 16:41:06> */
        //sig_coeff_flag[xC][yC] = cabac->significant_coeff_flag_decode(xC,yC,); //ae(v);
        if (sig_coeff_flag[xC][yC]) inferSbDcSigCoeffFlag = 0;
      }
    }
    int firstSigScanPos = 16;
    int lastSigScanPos = -1;
    int numGreater1Flag = 0;
    int lastGreater1ScanPos = -1;
    for (n = 15; n >= 0; n--) {
      xC = (xS << 2) + ScanOrder[2][scanIdx][n][0];
      yC = (yS << 2) + ScanOrder[2][scanIdx][n][1];
      if (sig_coeff_flag[xC][yC]) {
        if (numGreater1Flag < 8) {
          //coeff_abs_level_greater1_flag[n] = ae(v);
          std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__
                    << std::endl;
          exit(0);
          numGreater1Flag++;
          if (coeff_abs_level_greater1_flag[n] && lastGreater1ScanPos == -1)
            lastGreater1ScanPos = n;
          else if (coeff_abs_level_greater1_flag[n])
            escapeDataPresent = 1;
        } else
          escapeDataPresent = 1;
        if (lastSigScanPos == -1) lastSigScanPos = n;
        firstSigScanPos = n;
      }
    }

    int predModeIntra =
        (cIdx == 0) ? m_tu.intra_pred_mode : m_tu.intra_pred_mode_c;

    uint8_t coeff_abs_level_greater2_flag[32] = {0};
    uint8_t coeff_abs_level_remaining[32] = {0};
    uint8_t TransCoeffLevel[8][8][8][8][8] = {0};
    int signHidden;
    if (cu_transquant_bypass_flag ||
        (CuPredMode[x0][y0] == MODE_INTRA &&
         m_sps->implicit_rdpcm_enabled_flag &&
         transform_skip_flag[x0][y0][cIdx] &&
         (predModeIntra == 10 || predModeIntra == 26)) ||
        explicit_rdpcm_flag[x0][y0][cIdx])
      signHidden = 0;
    else
      signHidden = lastSigScanPos - firstSigScanPos > 3;
    if (lastGreater1ScanPos != -1) {
      //coeff_abs_level_greater2_flag[lastGreater1ScanPos] = ae(v);
      std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__ << std::endl;
      exit(0);
      if (coeff_abs_level_greater2_flag[lastGreater1ScanPos])
        escapeDataPresent = 1;
    }
    uint8_t coeff_sign_flag[16] = {0};
    for (n = 15; n >= 0; n--) {
      xC = (xS << 2) + ScanOrder[2][scanIdx][n][0];
      yC = (yS << 2) + ScanOrder[2][scanIdx][n][1];
      if (sig_coeff_flag[xC][yC] && (!m_pps->sign_data_hiding_enabled_flag ||
                                     !signHidden || (n != firstSigScanPos))) {
        std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__
                  << std::endl;
        exit(0);
        //coeff_sign_flag[n] = ae(v);
      }
    }
    int numSigCoeff = 0, sumAbsLevel = 0;
    for (n = 15; n >= 0; n--) {
      xC = (xS << 2) + ScanOrder[2][scanIdx][n][0];
      yC = (yS << 2) + ScanOrder[2][scanIdx][n][1];
      if (sig_coeff_flag[xC][yC]) {
        int baseLevel = 1 + coeff_abs_level_greater1_flag[n] +
                        coeff_abs_level_greater2_flag[n];
        if (baseLevel ==
            ((numSigCoeff < 8) ? ((n == lastGreater1ScanPos) ? 3 : 2) : 1)) {
          //coeff_abs_level_remaining[n] = ae(v);
          std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__
                    << std::endl;
          exit(0);
        }
        std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__
                  << std::endl;
        exit(0);
        //TransCoeffLevel[x0][y0][cIdx][xC][yC] =
        //(coeff_abs_level_remaining[n] + baseLevel) *
        //(1 - 2 * coeff_sign_flag[n]);
        if (m_pps->sign_data_hiding_enabled_flag && signHidden) {
          sumAbsLevel += (coeff_abs_level_remaining[n] + baseLevel);
          if ((n == firstSigScanPos) && ((sumAbsLevel % 2) == 1))
            TransCoeffLevel[x0][y0][cIdx][xC][yC] =
                -TransCoeffLevel[x0][y0][cIdx][xC][yC];
        }
        numSigCoeff++;
      }
    }
  }
  return 0;
}

int SliceData::cross_comp_pred(int x0, int y0, int c) {
  log2_res_scale_abs_plus1[c] = cabac->ff_hevc_log2_res_scale_abs(c); //ae(v);
  if (log2_res_scale_abs_plus1[c] != 0)
    res_scale_sign_flag[c] = cabac->ff_hevc_res_scale_sign_flag(c); // ae(v);
  return 0;
}

int SliceData::palette_coding(int x0, int y0, int nCbS) {
  std::cout << "Into -> " << __FUNCTION__ << "():" << __LINE__ << std::endl;
  exit(0);
  //  palettePredictionFinished = 0;
  //  NumPredictedPaletteEntries = 0;
  //  for (predictorEntryIdx = 0;
  //       predictorEntryIdx < PredictorPaletteSize && !palettePredictionFinished &&
  //       NumPredictedPaletteEntries < palette_max_size;
  //       predictorEntryIdx++) {
  //    palette_predictor_run = ae(v);
  //    if (palette_predictor_run != 1) {
  //      if (palette_predictor_run > 1)
  //        predictorEntryIdx += palette_predictor_run - 1;
  //      PalettePredictorEntryReuseFlags[predictorEntryIdx] = 1;
  //      NumPredictedPaletteEntries++
  //    } else
  //      palettePredictionFinished = 1;
  //  }
  //  if (NumPredictedPaletteEntries < palette_max_size)
  //    num_signalled_palette_entries = ae(v);
  //  numComps = (ChromaArrayType == 0) ? 1 : 3;
  //  for (cIdx = 0; cIdx < numComps; cIdx++)
  //    for (i = 0; i < num_signalled_palette_entries; i++)
  //      new_palette_entries[cIdx][i] = ae(v);
  //  if (CurrentPaletteSize != 0) palette_escape_val_present_flag = ae(v);
  //  if (MaxPaletteIndex > 0) {
  //    num_palette_indices_minus1 = ae(v);
  //    adjust = 0;
  //    for (i = 0; i <= num_palette_indices_minus1; i++) {
  //      if (MaxPaletteIndex - adjust > 0) {
  //        palette_idx_idc = ae(v);
  //        PaletteIndexIdc[i] = palette_idx_idc;
  //      }
  //      adjust = 1;
  //    }
  //    copy_above_indices_for_final_run_flag = ae(v);
  //    palette_transpose_flag = ae(v);
  //  }
  //  if (palette_escape_val_present_flag) {
  //    delta_qp();
  //    if (!cu_transquant_bypass_flag) chroma_qp_offset();
  //  }
  //  remainingNumIndices = num_palette_indices_minus1 + 1;
  //  PaletteScanPos = 0;
  //  log2BlockSize = Log2(nCbS);
  //  while (PaletteScanPos < nCbS * nCbS) {
  //    xC = x0 + ScanOrder[log2BlockSize][3][PaletteScanPos][0];
  //    yC = y0 + ScanOrder[log2BlockSize][3][PaletteScanPos][1];
  //    if (PaletteScanPos > 0) {
  //      xcPrev = x0 + ScanOrder[log2BlockSize][3][PaletteScanPos - 1][0];
  //      ycPrev = y0 + ScanOrder[log2BlockSize][3][PaletteScanPos - 1][1]
  //    }
  //    PaletteRunMinus1 = nCbS * nCbS - PaletteScanPos - 1;
  //    RunToEnd = 1 CopyAboveIndicesFlag[xC][yC] = 0;
  //    if (MaxPaletteIndex > 0)
  //      if (PaletteScanPos >= nCbS && CopyAboveIndicesFlag[xcPrev][ycPrev] == 0)
  //        if (remainingNumIndices > 0 && PaletteScanPos < nCbS * nCbS - 1) {
  //          copy_above_palette_indices_flag = ae(v);
  //          CopyAboveIndicesFlag[xC][yC] = copy_above_palette_indices_flag
  //        } else if (PaletteScanPos == nCbS * nCbS - 1 && remainingNumIndices > 0)
  //          CopyAboveIndicesFlag[xC][yC] = 0;
  //        else
  //          CopyAboveIndicesFlag[xC][yC] = 1;
  //    if (CopyAboveIndicesFlag[xC][yC] == 0) {
  //      currNumIndices = num_palette_indices_minus1 + 1 - remainingNumIndices;
  //      CurrPaletteIndex = PaletteIndexIdc[currNumIndices];
  //    }
  //    if (MaxPaletteIndex > 0) {
  //      if (CopyAboveIndicesFlag[xC][yC] == 0) remainingNumIndices - = 1;
  //      if (remainingNumIndices > 0 ||
  //          CopyAboveIndicesFlag[xC][yC] !=
  //              copy_above_indices_for_final_run_flag) {
  //        PaletteMaxRunMinus1 = nCbS * nCbS - PaletteScanPos - 1 -
  //                              remainingNumIndices -
  //                              copy_above_indices_for_final_run_flag;
  //        RunToEnd = 0;
  //        if (PaletteMaxRunMinus1 > 0) {
  //          palette_run_prefix = ae(v);
  //          if ((palette_run_prefix > 1) &&
  //              (PaletteMaxRunMinus1 != (1 << (palette_run_prefix - 1))))
  //            palette_run_suffix = ae(v);
  //        }
  //      }
  //    }
  //    runPos = 0 while (runPos <= PaletteRunMinus1) {
  //      xR = x0 + ScanOrder[log2BlockSize][3][PaletteScanPos][0];
  //      yR = y0 + ScanOrder[log2BlockSize][3][PaletteScanPos][1];
  //      if (CopyAboveIndicesFlag[xC][yC] == 0) {
  //        CopyAboveIndicesFlag[xR][yR] = 0;
  //        PaletteIndexMap[xR][yR] = CurrPaletteIndex;
  //      } else {
  //        CopyAboveIndicesFlag[xR][yR] = 1;
  //        PaletteIndexMap[xR][yR] = PaletteIndexMap[xR][yR - 1];
  //      }
  //      runPos++;
  //      PaletteScanPos++;
  //    }
  //  }
  //  if (palette_escape_val_present_flag) {
  //    for (cIdx = 0; cIdx < numComps; cIdx++)
  //      for (sPos = 0; sPos < nCbS * nCbS; sPos++) {
  //        xC = x0 + ScanOrder[log2BlockSize][3][sPos][0];
  //        yC = y0 + ScanOrder[log2BlockSize][3][sPos][1];
  //        if (PaletteIndexMap[xC][yC] == MaxPaletteIndex)
  //          if (cIdx == 0 | |
  //                  (xC % 2 == 0 && yC % 2 == 0 && ChromaArrayType == 1) ||
  //              (xC % 2 == 0 && !palette_transpose_flag &&
  //               ChromaArrayType == 2) ||
  //              (yC % 2 == 0 && palette_transpose_flag && ChromaArrayType == 2) ||
  //              ChromaArrayType == 3) {
  //            palette_escape_val = ae(v);
  //            PaletteEscapeVal[cIdx][xC][yC] = palette_escape_val;
  //          }
  //      }
  //  }
  return 0;
}
//
int SliceData::delta_qp() {
  if (m_pps->cu_qp_delta_enabled_flag && !IsCuQpDeltaCoded) {
    IsCuQpDeltaCoded = 1;
    int cu_qp_delta_abs = cabac->ff_hevc_cu_qp_delta_abs(); //ae(v);
    if (cu_qp_delta_abs) {
      int cu_qp_delta_sign_flag = cabac->ff_decode_bypass(); //ae(v);
      CuQpDeltaVal = cu_qp_delta_abs * (1 - 2 * cu_qp_delta_sign_flag);
    }
  }
  return 0;
}

int SliceData::chroma_qp_offset() {
  if (header->cu_chroma_qp_offset_enabled_flag && !IsCuChromaQpOffsetCoded) {
    int cu_chroma_qp_offset_flag =
        cabac->decode_bin(elem_offset[CU_CHROMA_QP_OFFSET_FLAG]); //ae(v);
    if (cu_chroma_qp_offset_flag && m_pps->chroma_qp_offset_list_len_minus1 > 0)
      int cu_chroma_qp_offset_idx = cabac->ff_hevc_cu_chroma_qp_offset_idx(
          m_pps->chroma_qp_offset_list_len_minus1); //ae(v);
  }
  return 0;
}

int SliceData::slice_decoding_process() {
  /* 在场编码时可能存在多个slice data，只需要对首个slice data进行定位，同时在下面的操作中，只需要在首次进入Slice时才需要执行的 */
  if (pic->m_slice_cnt == 0) {
    /* 解码参考帧重排序(POC) */
    // 8.2.1 Decoding process for picture order count
    pic->decoding_picture_order_count(m_sps->pic_order_cnt_type);
    if (m_sps->frame_mbs_only_flag == 0) {
      /* 存在场宏块 */
      pic->m_parent->m_picture_top_filed.copyDataPicOrderCnt(*pic);
      //顶（底）场帧有可能被选为参考帧，在解码P/B帧时，会用到PicOrderCnt字段，所以需要在此处复制一份
      pic->m_parent->m_picture_bottom_filed.copyDataPicOrderCnt(*pic);
    }

    /* 8.2.2 Decoding process for macroblock to slice group map */
    decoding_macroblock_to_slice_group_map();

    // 8.2.4 Decoding process for reference picture lists construction
    if (header->slice_type == SLICE_P || header->slice_type == SLICE_SP ||
        header->slice_type == SLICE_B) {
      /* 当前帧需要参考帧预测，则需要进行参考帧重排序。在每个 P、SP 或 B Slice的解码过程开始时调用 */
      pic->decoding_ref_picture_lists_construction(
          pic->m_dpb, pic->m_RefPicList0, pic->m_RefPicList1);

      /* (m_RefPicList0,m_RefPicList1为m_dpb排序后的前后参考列表）打印帧重排序先后信息 */
      printFrameReorderPriorityInfo();
    }
  }
  pic->m_slice_cnt++;
  return 0;
}

// 8.2.2 Decoding process for macroblock to slice group map
/* 输入:活动图像参数集和要解码的Slice header。  
 * 输出:宏块到Slice Group映射MbToSliceGroupMap。 */
//该过程在每个Slice开始时调用（如果是单帧由单个Slice组成的情况，那么这里几乎没有逻辑）
inline int SliceData::decoding_macroblock_to_slice_group_map() {
  //输出为：mapUnitToSliceGroupMap
  mapUnitToSliceGroupMap();
  //输入为：mapUnitToSliceGroupMap
  mbToSliceGroupMap();
  return 0;
}

//8.2.2.1 - 8.2.2.7  Specification for interleaved slice group map type
inline int SliceData::mapUnitToSliceGroupMap() {
  /* 输入 */
  const int &MapUnitsInSliceGroup0 = header->MapUnitsInSliceGroup0;
  /* 输出 */
  int32_t *&mapUnitToSliceGroupMap = header->mapUnitToSliceGroupMap;

  /* mapUnitToSliceGroupMap 数组的推导如下：
   * – 如果 num_slice_groups_minus1 等于 0，则为范围从 0 到 PicSizeInMapUnits - 1（含）的所有 i 生成Slice Group映射的映射单元，如 mapUnitToSliceGroupMap[ i ] = 0 */
  /* 整个图像只被分为一个 slice group */
  if (m_pps->num_slice_groups_minus1 == 0) {
    /* 这里按照一个宏块或宏块对（当为MBAFF时，遍历大小减小一半）处理 */
    for (int i = 0; i < (int)m_sps->PicSizeInMapUnits; i++)
      /* 确保在只有一个Slice组的情况下，整个图像的所有宏块都被正确地映射到这个唯一的Slice组上，简化处理逻辑这里赋值不一定非要为0,只要保持映射单元内都是同一个值就行了 */
      mapUnitToSliceGroupMap[i] = 0;
    /* TODO YangJing 有问题，如果是场编码，那么这里实际上只处理了一半映射 <24-09-16 00:07:20> */
    return 0;
  }

  /* TODO YangJing 这里还没测过，怎么造一个多Slice文件？ <24-09-15 23:25:12> */

  /* — 否则（num_slice_groups_minus1 不等于0），mapUnitToSliceGroupMap 的推导如下： 
       * — 如果slice_group_map_type 等于0，则应用第8.2.2.1 节中指定的mapUnitToSliceGroupMap 的推导。  
       * — 否则，如果slice_group_map_type等于1，则应用第8.2.2.2节中指定的mapUnitToSliceGroupMap的推导。  
       * — 否则，如果slice_group_map_type等于2，则应用第8.2.2.3节中指定的mapUnitToSliceGroupMap的推导。
       * – 否则，如果slice_group_map_type等于3，则应用第8.2.2.4节中指定的mapUnitToSliceGroupMap的推导。  
       * — 否则，如果slice_group_map_type等于4，则应用第8.2.2.5节中指定的mapUnitToSliceGroupMap的推导。  
       * – 否则，如果slice_group_map_type等于5，则应用第8.2.2.6节中指定的mapUnitToSliceGroupMap的推导。  
       * — 否则（slice_group_map_type 等于 6），应用第 8.2.2.7 节中指定的 mapUnitToSliceGroupMap 的推导。*/

  switch (m_pps->slice_group_map_type) {
  case 0:
    interleaved_slice_group_map_type(mapUnitToSliceGroupMap);
    break;
  case 1:
    dispersed_slice_group_map_type(mapUnitToSliceGroupMap);
    break;
  case 2:
    foreground_with_left_over_slice_group_ma_type(mapUnitToSliceGroupMap);
    break;
  case 3:
    box_out_slice_group_map_types(mapUnitToSliceGroupMap,
                                  MapUnitsInSliceGroup0);
    break;
  case 4:
    raster_scan_slice_group_map_types(mapUnitToSliceGroupMap,
                                      MapUnitsInSliceGroup0);
    break;
  case 5:
    wipe_slice_group_map_types(mapUnitToSliceGroupMap, MapUnitsInSliceGroup0);
    break;
  default:
    explicit_slice_group_map_type(mapUnitToSliceGroupMap);
    break;
  }
  return 0;
}

// 8.2.2.8 Specification for conversion of map unit to slice group map to macroblock to slice group map
/* 宏块（Macroblock）的位置映射到Slice（Slice）的过程*/
inline int SliceData::mbToSliceGroupMap() {
  /* 输入：存储每个宏块单元对应的Slice Group索引，在A Frame = A Slice的情况下，这里均为0 */
  const int32_t *mapUnitToSliceGroupMap = header->mapUnitToSliceGroupMap;

  /* 输出：存储映射后每个宏块对应的Slice Group索引 */
  int32_t *&MbToSliceGroupMap = header->MbToSliceGroupMap;

  /* 对于Slice中的每个宏块（若是场编码，顶、底宏块也需要单独遍历），宏块到Slice Group映射指定如下： */
  for (int mbIndex = 0; mbIndex < header->PicSizeInMbs; mbIndex++) {
    if (m_sps->frame_mbs_only_flag || header->field_pic_flag)
      /* 对于一个全帧或全场（即不存在混合帧、场），每个宏块独立对应一个映射单位 */
      MbToSliceGroupMap[mbIndex] = mapUnitToSliceGroupMap[mbIndex];
    else if (MbaffFrameFlag)
      /* 映射基于宏块对，一个宏块对（2个宏块）共享一个映射单位 */
      MbToSliceGroupMap[mbIndex] = mapUnitToSliceGroupMap[mbIndex / 2];
    else {
      /* 场编码的交错模式，每个宏块对跨越两行，每一对宏块（通常包括顶部场和底部场的宏块）被当作一个单元处理 */
      /*   +-----------+        +-----------+
           |           |        | top filed |
           |           |        | btm filed |
           | A  Slice  |   -->  | top filed | 
           |           |        | btm filed |
           |           |        | top filed |
           |           |        | btm filed |
           +-----------+        +-----------+*/
      /* 用宏块索引对图像宽度取模，得到该宏块在其行中的列位置 */
      uint32_t x = mbIndex % m_sps->PicWidthInMbs;
      /* 因为每个宏块对占据两行（每行一个宏块），所以用宽度的两倍来计算行号，得到顶宏块的行数 */
      uint32_t y = mbIndex / (2 * m_sps->PicWidthInMbs);
      MbToSliceGroupMap[mbIndex] =
          mapUnitToSliceGroupMap[y * m_sps->PicWidthInMbs + x];
    }
  }

  return 0;
}

/* mb_skip_run指定连续跳过的宏块的数量，在解码P或SPSlice时，MB_type应被推断为P_Skip并且宏块类型统称为P宏块类型，或者在解码BSlice时， MB_type应被推断为B_Skip，并且宏块类型统称为B宏块类型 */
int SliceData::process_mb_skip_run(int32_t &prevMbSkipped) {
  mb_skip_run = bs->readUE();

  /* 当mb_skip_run > 0时，实际上在接下来的宏块解析中，将要跳过处理，所以能够表示prevMbSkipped = 1*/
  prevMbSkipped = mb_skip_run > 0;

  /* 对于每个跳过的宏块，不同于CABAC的处理，这里是处理多个宏块，而CABAC是处理单个宏块 */
  for (uint32_t i = 0; i < mb_skip_run; i++) {
    /* 1. 计算当前宏块的位置 */
    updatesLocationOfCurrentMacroblock(MbaffFrameFlag);

    /* 2. 对于跳过的宏块同样需要增加实际的宏块计数 */
    pic->mb_cnt++;

    /* 3. 对于MBAFF模式下，顶宏块需要先确定当前是否为帧宏块还是场宏块，以确定下面的帧间预测中如何处理宏块对 */
    if (MbaffFrameFlag && CurrMbAddr % 2 == 0)
      //这里是推导（根据前面已解码的宏块进行推导当前值），而不是解码
      derivation_for_mb_field_decoding_flag();

    /* 4. 宏块层也需要将该宏块该宏块的类型设置“Skip类型” */
    pic->m_mbs[pic->CurrMbAddr].decode_skip(*pic, *this, *cabac);

    /* 5. 由于跳过的宏块只能是P,B Slice中的宏块，那么就只需要调用帧间预测 */
    pic->inter_prediction_process();

    /* 6. 外层循环也需要更新宏块跳过地址 */
    CurrMbAddr = NextMbAddress(CurrMbAddr, header);
  }
  return 0;
}

/* 更新当前宏快位置 */
/* 输入：PicWidthInMbs,MbaffFrameFlag
 * 输出：mb_x, mb_y, CurrMbAddr*/
void SliceData::updatesLocationOfCurrentMacroblock(const bool MbaffFrameFlag) {
  const uint32_t h = MbaffFrameFlag ? 2 : 1;
  const uint32_t w = pic->PicWidthInMbs * h; //当为宏块对模式时，实际上$W = 2*W$
  pic->mb_x = (CurrMbAddr % w) / h;
  pic->mb_y = (CurrMbAddr / w * h) + ((CurrMbAddr % w) % h);
  pic->CurrMbAddr = CurrMbAddr;
}

/* 如果当前宏块的运动矢量与参考帧中的预测块非常接近，且残差（即当前块与预测块的差异）非常小或为零，编码器可能会选择跳过该宏块的编码。
 * 在这种情况下，解码器可以通过运动矢量预测和参考帧直接重建宏块，而无需传输额外的残差信息。
 * 一般来说，在I帧中，不会出现宏块跳过处理。这是因为I帧中的宏块是使用帧内预测进行编码的，而不是基于参考帧的帧间预测 */
int SliceData::process_mb_skip_flag(const int32_t prevMbSkipped) {
  /* 1. 计算当前宏块的位置 */
  updatesLocationOfCurrentMacroblock(MbaffFrameFlag);

  MacroBlock &curr_mb = pic->m_mbs[CurrMbAddr];
  MacroBlock &next_mb = pic->m_mbs[CurrMbAddr + 1];

  /* 2. 设置当前宏块的Slice编号 */
  curr_mb.slice_number = slice_number;

  /* 3. 当前帧是MBAFF帧，且顶宏块和底宏块的场解码标志都未设置，则推导出mb_field_decoding_flag的值 */
  if (MbaffFrameFlag) {
    /* 当 mb_field_decoding_flag 未设置，需要推导其初始值，根据相邻“宏块对”来推导当前宏块类型，若为首个宏块(0,0)则设置为帧宏块。（不能理解为"当宏块对作为帧宏块处理时"）*/
    if (CurrMbAddr % 2 == 0 &&
        (!curr_mb.mb_field_decoding_flag && !next_mb.mb_field_decoding_flag))
      derivation_for_mb_field_decoding_flag();

    curr_mb.mb_field_decoding_flag = mb_field_decoding_flag;
  }

  /* 4. 在当前宏块对中，若首宏块跳过处理，则次宏块跳过标记设置为下个宏块对的首宏块一致。NOTE:这里是CABAC的一个特殊规则，为了减少比特流中需要传输的标志位数量 */
  if (MbaffFrameFlag && CurrMbAddr % 2 == 1 && prevMbSkipped)
    mb_skip_flag = mb_skip_flag_next_mb;

  /* 若非MBAFF模式，则直接解码获取是否需要跳过该宏块*/
  else
    cabac->decode_mb_skip_flag(CurrMbAddr, mb_skip_flag);

  /* 5. 宏块需要进行跳过处理时：由于宏块跳过时，一般来说没有残差数据，则需要运动矢量预测和参考帧直接重建宏块（帧间预测,P,B Slice） */
  if (mb_skip_flag && header->slice_type != SLICE_I &&
      header->slice_type != SLICE_SI) {
    pic->mb_cnt++;

    /* 若为MBAFF模式，则宏块对需要一并处理 */
    if (MbaffFrameFlag && CurrMbAddr % 2 == 0) {
      curr_mb.mb_skip_flag = mb_skip_flag;

      // 次宏块与首宏块的宏块对是相同的（要么都是帧宏块，要么都是场宏块），进行宏块对的同步
      next_mb.slice_number = slice_number;
      next_mb.mb_field_decoding_flag = mb_field_decoding_flag;

      /* 解码次宏块是否需要跳过宏块处理（由于宏块对是一起操作的，所以这里允许先解码下一个宏块）*/
      cabac->decode_mb_skip_flag(CurrMbAddr + 1, mb_skip_flag_next_mb);

      /* 当首宏块跳过处理，但次宏块需要解码 */
      if (mb_skip_flag_next_mb == 0) {
        cabac->decode_mb_field_decoding_flag(mb_field_decoding_flag);
        is_mb_field_decoding_flag_prcessed = true;
      }
      /* 当首、次宏块均跳过处理时，需根据标准规定，要再次推导当前首宏块的宏块对解码类型，确保宏块对类型正确（与上面一次调用不同的是，CABAC解码需要根据最新的上下文重新推导
       * NOTE: 这里有一个情况是当一个Slice被分为奇数行宏块时，那么在MBAFF模式下，宏块对的划分后，最后一行宏块会被当作首宏块处理，且需要与一个虚拟的底宏块组成宏块对进行处理，虚拟的底宏块会被视为跳过的宏块，不包含任何数据。 */
      else
        derivation_for_mb_field_decoding_flag();
    }

    /* 宏块层对应的处理 */
    curr_mb.decode_skip(*pic, *this, *cabac);

    /* 由于跳过的宏块只能是P,B Slice中的宏块，那么就只需要调用帧间预测 */
    pic->inter_prediction_process(); // 帧间预测
  }

  return 0;
}

//解码mb_field_decoding_flag: 表示本宏块对是帧宏块对，还是场宏块对
int SliceData::process_mb_field_decoding_flag(bool entropy_coding_mode_flag) {
  int ret = 0;
  if (is_mb_field_decoding_flag_prcessed) {
    is_mb_field_decoding_flag_prcessed = false;
    return 1;
  }

  if (entropy_coding_mode_flag)
    ret = cabac->decode_mb_field_decoding_flag(mb_field_decoding_flag);
  else
    mb_field_decoding_flag = bs->readU1();

  RET(ret);
  return 0;
}

int SliceData::process_end_of_slice_flag(int32_t &end_of_slice_flag) {
  cabac->decode_end_of_slice_flag(end_of_slice_flag);
  return 0;
}

/* mb_field_decoding_flag. ->  Rec. ITU-T H.264 (08/2021) 98*/
//该函数只适用于MBAFF模式下调用，根据相邻“宏块对”的状态来推断当前“宏块对”的 mb_field_decoding_flag 值
int SliceData::derivation_for_mb_field_decoding_flag() {
  /* NOTE:在同一Slice内，编码模式只能是一种，若存在左邻宏块或上邻宏块则编码模式必然一致（只能通过左、上方向推导、这是扫描顺序决定的） */
  /* 同一片中当前宏块对的左侧或上方不存在相邻宏块对，即默认为帧宏块 */
  mb_field_decoding_flag = 0;
  if (pic->mb_x > 0) {
    /* 存在左侧的相邻宏块对，且属于同一Slice group，则直接copy */
    auto &left_mb = pic->m_mbs[CurrMbAddr - 2];
    if (left_mb.slice_number == slice_number)
      mb_field_decoding_flag = left_mb.mb_field_decoding_flag;
  } else if (pic->mb_y > 0) {
    /* 存在上侧的相邻宏块对，且属于同一Slice group，则直接copy */
    auto &top_mb = pic->m_mbs[CurrMbAddr - 2 * pic->PicWidthInMbs];
    if (top_mb.slice_number == slice_number)
      mb_field_decoding_flag = top_mb.mb_field_decoding_flag;
  }
  return 0;
}

int SliceData::do_macroblock_layer() {
  /* 1. 计算当前宏块的位置 */
  updatesLocationOfCurrentMacroblock(MbaffFrameFlag);
  /* 2. 在宏块层中对每个宏块处理或解码得到对应帧内、帧间解码所需要的信息*/
  pic->m_mbs[pic->CurrMbAddr].decode(*bs, *pic, *this, *cabac);
  pic->mb_cnt++;
  return 0;
}

int SliceData::decoding_process() {
  /* ------------------ 设置别名 ------------------ */
  const int32_t &picWidthInSamplesL = pic->PicWidthInSamplesL;
  const int32_t &picWidthInSamplesC = pic->PicWidthInSamplesC;
  const int32_t &BitDepth = pic->m_slice->slice_header->m_sps->BitDepthY;

  uint8_t *&pic_buff_luma = pic->m_pic_buff_luma;
  uint8_t *&pic_buff_cb = pic->m_pic_buff_cb;
  uint8_t *&pic_buff_cr = pic->m_pic_buff_cr;

  MacroBlock &mb = pic->m_mbs[pic->CurrMbAddr];
  /* ------------------  End ------------------ */

  //8.5 Transform coefficient decoding process and picture construction process prior to deblocking filter process（根据不同类型的预测模式，进行去块滤波处理之前的变换系数解码处理和图片构造处理 ）
  bool isNeedIntraPrediction = true;
  //----------------------------------- 帧内预测 -----------------------------------
  if (mb.m_mb_pred_mode == Intra_4x4) //分区预测，处理最为复杂的高纹理区域
    pic->transform_decoding_for_4x4_luma_residual_blocks(
        0, 0, BitDepth, picWidthInSamplesL, pic_buff_luma);
  else if (mb.m_mb_pred_mode == Intra_8x8) //分区预测
    pic->transform_decoding_for_8x8_luma_residual_blocks(
        0, 0, BitDepth, picWidthInSamplesL, mb.LumaLevel8x8, pic_buff_luma);
  else if (mb.m_mb_pred_mode == Intra_16x16) //整块预测，处理较为简单的区域
    pic->transform_decoding_for_luma_samples_of_16x16(
        0, BitDepth, mb.QP1Y, picWidthInSamplesL, mb.Intra16x16DCLevel,
        mb.Intra16x16ACLevel, pic_buff_luma);
  //----------------------------------- 原始数据 -----------------------------------
  else if (mb.m_name_of_mb_type == I_PCM) {
    pic->sample_construction_for_I_PCM();
    goto eof;
  }
  //----------------------------------- 帧间预测 -----------------------------------
  else {
    // 对于帧间预测而言，过程中不需要调用帧内预测
    isNeedIntraPrediction = false;
    pic->inter_prediction_process();
    /* 选择 4x4 或 8x8 的残差块解码函数来处理亮度残差块 */
    if (mb.transform_size_8x8_flag)
      pic->transform_decoding_for_8x8_luma_residual_blocks(
          0, 0, BitDepth, picWidthInSamplesL, mb.LumaLevel8x8, pic_buff_luma,
          false);
    else
      pic->transform_decoding_for_4x4_luma_residual_blocks(
          0, 0, BitDepth, picWidthInSamplesL, pic_buff_luma, false);
  }

  /* 帧内、帧间预测都调用，当存在色度采样时，即YUV420,YUV422,YUV444进行色度解码; 反之，则不进行色度解码 */
  if (m_sps->ChromaArrayType) {
    pic->transform_decoding_for_chroma_samples(
        1, picWidthInSamplesC, pic_buff_cb, isNeedIntraPrediction);
    pic->transform_decoding_for_chroma_samples(
        0, picWidthInSamplesC, pic_buff_cr, isNeedIntraPrediction);
  }

  /* 至此该宏块的原始数据完成全部解码工作，输出的pic_buff_luma,pic_buff_cb,pic_buff_cr即为解码的原始数据 */
eof:
  return 0;
}

// 第 8.2.2.8 节的规定导出宏块到Slice Group映射后，该函数导出NextMbAddress的值
/* 跳过不属于当前Slice Group的宏块，找到与当前宏块位于同一Slice Group中的下一个宏块 */
int NextMbAddress(int currMbAddr, SliceHeader *header) {
  int nextMbAddr = currMbAddr + 1;

  /* 宏块索引不应该为负数或大于该Slice的总宏块数 + 1 */
  while (nextMbAddr < header->PicSizeInMbs &&
         header->MbToSliceGroupMap[nextMbAddr] !=
             header->MbToSliceGroupMap[currMbAddr]) {
    /* 下一个宏块是否与当前宏块位于同一个Slice Group中。如果不在同一个Slice Group中，则继续增加 nextMbAddr 直到找到属于同一个Slice Group的宏块。*/
    nextMbAddr++;
  }

  if (nextMbAddr < 0)
    cerr << "An error occurred CurrMbAddr:" << nextMbAddr << " on "
         << __FUNCTION__ << "():" << __LINE__ << endl;
  return nextMbAddr;
}

void SliceData::printFrameReorderPriorityInfo() {
  string sliceType = "UNKNOWN";
  cout << "\tGOP[" << pic->m_PicNumCnt + 1 << "] -> {" << endl;
  for (int i = 0; i < MAX_DPB; ++i) {
    const auto &refPic = pic->m_dpb[i];
    if (refPic) {
      auto &frame = refPic->m_picture_frame;
      auto &sliceHeader = frame.m_slice->slice_header;
      if (frame.m_slice == nullptr) continue;
      if (sliceHeader->slice_type != SLICE_I && frame.PicOrderCnt == 0 &&
          frame.PicNum == 0 && frame.m_PicNumCnt == 0)
        continue;
      sliceType = H264_SLIECE_TYPE_TO_STR(sliceHeader->slice_type);
      if (pic->PicOrderCnt == frame.PicOrderCnt)
        cout << "\t\t* DPB[" << i << "]: ";
      else
        cout << "\t\t  DPB[" << i << "]: ";
      cout << sliceType << "; POC(显示顺序)=" << frame.PicOrderCnt
           << "; frame_num(帧编号，编码顺序)="
           << frame.m_slice->slice_header->frame_num << ";\n";
    }
  }
  cout << "\t}" << endl;

  if (header->slice_type == SLICE_P || header->slice_type == SLICE_SP)
    cout << "\t当前帧所参考帧列表(按frame_num排序) -> {" << endl;
  else if (header->slice_type == SLICE_B)
    cout << "\t当前帧所参考帧列表(按POC排序) -> {" << endl;

  for (uint32_t i = 0; i < pic->m_RefPicList0Length; ++i) {
    const auto &refPic = pic->m_RefPicList0[i];
    if (refPic && (refPic->reference_marked_type == SHORT_REF ||
                   refPic->reference_marked_type == LONG_REF)) {
      auto &frame = refPic->m_picture_frame;
      auto &sliceHeader = frame.m_slice->slice_header;

      sliceType = H264_SLIECE_TYPE_TO_STR(sliceHeader->slice_type);
      cout << "\t\t(前参考)RefPicList0[" << i << "]: " << sliceType
           << "; POC(显示顺序)=" << frame.PicOrderCnt
           << "; frame_num(帧编号，编码顺序)=" << frame.PicNum << ";\n";
    }
  }

  for (uint32_t i = 0; i < pic->m_RefPicList1Length; ++i) {
    const auto &refPic = pic->m_RefPicList1[i];
    if (refPic && (refPic->reference_marked_type == SHORT_REF ||
                   refPic->reference_marked_type == LONG_REF)) {
      auto &frame = refPic->m_picture_frame;
      auto &sliceHeader = frame.m_slice->slice_header;

      sliceType = H264_SLIECE_TYPE_TO_STR(sliceHeader->slice_type);
      cout << "\t\t(后参考)RefPicList1[" << i << "]: " << sliceType
           << "; POC(显示顺序)=" << frame.PicOrderCnt
           << "; frame_num(帧编号，编码顺序)=" << frame.PicNum << ";\n";
    }
  }
  cout << "\t}" << endl;
}

//8.2.2.1 Specification for interleaved slice group map type
int SliceData::interleaved_slice_group_map_type(
    int32_t *&mapUnitToSliceGroupMap) {
  uint32_t i = 0;
  do {
    for (uint32_t iGroup = 0; iGroup <= m_pps->num_slice_groups_minus1 &&
                              i < m_sps->PicSizeInMapUnits;
         i += m_pps->run_length_minus1[iGroup++] + 1) {
      for (uint32_t j = 0; j <= m_pps->run_length_minus1[iGroup] &&
                           i + j < m_sps->PicSizeInMapUnits;
           j++) {
        mapUnitToSliceGroupMap[i + j] = iGroup;
      }
    }
  } while (i < m_sps->PicSizeInMapUnits);
  return 0;
}
//8.2.2.2 Specification for dispersed slice group map type
int SliceData::dispersed_slice_group_map_type(
    int32_t *&mapUnitToSliceGroupMap) {
  for (int i = 0; i < (int)m_sps->PicSizeInMapUnits; i++) {
    mapUnitToSliceGroupMap[i] =
        ((i % m_sps->PicWidthInMbs) +
         (((i / m_sps->PicWidthInMbs) * (m_pps->num_slice_groups_minus1 + 1)) /
          2)) %
        (m_pps->num_slice_groups_minus1 + 1);
  }
  return 0;
}
//8.2.2.3 Specification for foreground with left-over slice group map type
int SliceData::foreground_with_left_over_slice_group_ma_type(
    int32_t *&mapUnitToSliceGroupMap) {
  for (int i = 0; i < (int)m_sps->PicSizeInMapUnits; i++)
    mapUnitToSliceGroupMap[i] = m_pps->num_slice_groups_minus1;

  for (int iGroup = m_pps->num_slice_groups_minus1 - 1; iGroup >= 0; iGroup--) {
    int32_t yTopLeft = m_pps->top_left[iGroup] / m_sps->PicWidthInMbs;
    int32_t xTopLeft = m_pps->top_left[iGroup] % m_sps->PicWidthInMbs;
    int32_t yBottomRight = m_pps->bottom_right[iGroup] / m_sps->PicWidthInMbs;
    int32_t xBottomRight = m_pps->bottom_right[iGroup] % m_sps->PicWidthInMbs;
    for (int y = yTopLeft; y <= yBottomRight; y++) {
      for (int x = xTopLeft; x <= xBottomRight; x++) {
        mapUnitToSliceGroupMap[y * m_sps->PicWidthInMbs + x] = iGroup;
      }
    }
  }
  return 0;
}

//8.2.2.4 Specification for box-out slice group map types
int SliceData::box_out_slice_group_map_types(int32_t *&mapUnitToSliceGroupMap,
                                             const int &MapUnitsInSliceGroup0) {

  for (int i = 0; i < (int)m_sps->PicSizeInMapUnits; i++)
    mapUnitToSliceGroupMap[i] = 1;

  int x = (m_sps->PicWidthInMbs - m_pps->slice_group_change_direction_flag) / 2;
  int y =
      (m_sps->PicHeightInMapUnits - m_pps->slice_group_change_direction_flag) /
      2;

  int32_t leftBound = x;
  int32_t topBound = y;
  int32_t rightBound = x;
  int32_t bottomBound = y;
  int32_t xDir = m_pps->slice_group_change_direction_flag - 1;
  int32_t yDir = m_pps->slice_group_change_direction_flag;
  int32_t mapUnitVacant = 0;

  for (int k = 0; k < MapUnitsInSliceGroup0; k += mapUnitVacant) {
    mapUnitVacant = (mapUnitToSliceGroupMap[y * m_sps->PicWidthInMbs + x] == 1);
    if (mapUnitVacant) {
      mapUnitToSliceGroupMap[y * m_sps->PicWidthInMbs + x] = 0;
    }
    if (xDir == -1 && x == leftBound) {
      leftBound = fmax(leftBound - 1, 0);
      x = leftBound;
      xDir = 0;
      yDir = 2 * m_pps->slice_group_change_direction_flag - 1;
    } else if (xDir == 1 && x == rightBound) {
      rightBound = MIN(rightBound + 1, m_sps->PicWidthInMbs - 1);
      x = rightBound;
      xDir = 0;
      yDir = 1 - 2 * m_pps->slice_group_change_direction_flag;
    } else if (yDir == -1 && y == topBound) {
      topBound = MAX(topBound - 1, 0);
      y = topBound;
      xDir = 1 - 2 * m_pps->slice_group_change_direction_flag;
      yDir = 0;
    } else if (yDir == 1 && y == bottomBound) {
      bottomBound = MIN(bottomBound + 1, m_sps->PicHeightInMapUnits - 1);
      y = bottomBound;
      xDir = 2 * m_pps->slice_group_change_direction_flag - 1;
      yDir = 0;
    } else {
      //(x, y) = (x + xDir, y + yDir);
    }
  }
  return 0;
}
//8.2.2.5 Specification for raster scan slice group map types
int SliceData::raster_scan_slice_group_map_types(
    int32_t *&mapUnitToSliceGroupMap, const int &MapUnitsInSliceGroup0) {
  // 8.2.2.5 Specification for raster scan slice group map types
  // 栅格扫描型 slice 组映射类型的描述
  int32_t sizeOfUpperLeftGroup = 0;
  if (m_pps->num_slice_groups_minus1 == 1) {
    sizeOfUpperLeftGroup =
        (m_pps->slice_group_change_direction_flag
             ? (m_sps->PicSizeInMapUnits - MapUnitsInSliceGroup0)
             : MapUnitsInSliceGroup0);
  }

  for (int i = 0; i < (int)m_sps->PicSizeInMapUnits; i++) {
    if (i < sizeOfUpperLeftGroup)
      mapUnitToSliceGroupMap[i] = m_pps->slice_group_change_direction_flag;
    else
      mapUnitToSliceGroupMap[i] = 1 - m_pps->slice_group_change_direction_flag;
  }
  return 0;
}
//8.2.2.6 Specification for wipe slice group map types
int SliceData::wipe_slice_group_map_types(int32_t *&mapUnitToSliceGroupMap,
                                          const int &MapUnitsInSliceGroup0) {
  int32_t sizeOfUpperLeftGroup = 0;
  if (m_pps->num_slice_groups_minus1 == 1) {
    sizeOfUpperLeftGroup =
        (m_pps->slice_group_change_direction_flag
             ? (m_sps->PicSizeInMapUnits - MapUnitsInSliceGroup0)
             : MapUnitsInSliceGroup0);
  }

  int k = 0;
  for (int j = 0; j < (int)m_sps->PicWidthInMbs; j++) {
    for (int i = 0; i < (int)m_sps->PicHeightInMapUnits; i++) {
      if (k++ < sizeOfUpperLeftGroup) {
        mapUnitToSliceGroupMap[i * m_sps->PicWidthInMbs + j] =
            m_pps->slice_group_change_direction_flag;
      } else {
        mapUnitToSliceGroupMap[i * m_sps->PicWidthInMbs + j] =
            1 - m_pps->slice_group_change_direction_flag;
      }
    }
  }
  return 0;
}

//8.2.2.7 Specification for explicit slice group map type
int SliceData::explicit_slice_group_map_type(int32_t *&mapUnitToSliceGroupMap) {
  for (int i = 0; i < (int)m_sps->PicSizeInMapUnits; i++)
    mapUnitToSliceGroupMap[i] = m_pps->slice_group_id[i];
  return 0;
}

//6.5.3 Up-right diagonal scan order array initialization process
//该过程的输入是块大小 blkSize。
//该过程的输出是数组 diagScan[ sPos ][ sComp ]。数组索引 sPos 指定扫描位置，范围从 0 到 ( blkSize * blkSize ) − 1。数组索引 sComp 等于 0 指定水平分量，数组索引 sComp 等于 1 指定垂直分量。根据 blkSize 的值，数组 diagScan 的推导如下：
int SliceData::Up_right_diagonal_scan_order_array_initialization_process(
    int blkSize, uint8_t diagScan[16][2]) {
  int i = 0, x = 0, y = 0;
  bool stopLoop = false;
  while (!stopLoop) {
    while (y >= 0) {
      if (x < blkSize && y < blkSize) {
        diagScan[i][0] = x;
        diagScan[i][1] = y;
        i++;
      }
      y--;
      x++;
    }
    y = x;
    x = 0;
    if (i >= blkSize * blkSize) stopLoop = true;
  }
  return 0;
}

//6.5.4 Horizontal scan order array initialization process
//Input to this process is a block size blkSize.
//Output of this process is the array horScan[ sPos ][ sComp ]. The array index sPos specifies the scan position ranging from 0 to ( blkSize * blkSize ) − 1. The array index sComp equal to 0 specifies the horizontal component and the array index sComp equal to 1 specifies the vertical component. Depending on the value of blkSize, the array horScan is derived as follows:
int SliceData::Horizontal_scan_order_array_initialization_process(
    int blkSize, uint8_t horScan[16][2]) {
  int i = 0;
  for (int y = 0; y < blkSize; y++)
    for (int x = 0; x < blkSize; x++) {
      horScan[i][0] = x;
      horScan[i][1] = y;
      i++;
    }
  return 0;
}

//6.5.5 Vertical scan order array initialization process
//Input to this process is a block size blkSize.
//Output of this process is the array verScan[ sPos ][ sComp ]. The array index sPos specifies the scan position ranging from 0 to ( blkSize * blkSize ) − 1. The array index sComp equal to 0 specifies the horizontal component and the array index sComp equal to 1 specifies the vertical component. Depending on the value of blkSize, the array verScan is derived as follows:
int SliceData::Vertical_scan_order_array_initialization_process(
    int blkSize, uint8_t verScan[16][2]) {
  int i = 0;
  for (int x = 0; x < blkSize; x++)
    for (int y = 0; y < blkSize; y++) {
      verScan[i][0] = x;
      verScan[i][1] = y;
      i++;
    }
  return 0;
}

//6.5.6 Traverse scan order array initialization process
//Input to this process is a block size blkSize.
//Output of this process is the array travScan[ sPos ][ sComp ]. The array index sPos specifies the scan position ranging from 0 to ( blkSize * blkSize ) − 1, inclusive. The array index sComp equal to 0 specifies the horizontal component and the array index sComp equal to 1 specifies the vertical component. Depending on the value of blkSize, the array travScan is derived as follows:
int SliceData::Traverse_scan_order_array_initialization_process(
    int blkSize, uint8_t travScan[16][2]) {
  int i = 0;
  for (int y = 0; y < blkSize; y++)
    if (y % 2 == 0)
      for (int x = 0; x < blkSize; x++) {
        travScan[i][0] = x;
        travScan[i][1] = y;
        i++;
      }
    else
      for (int x = blkSize - 1; x >= 0; x--) {
        travScan[i][0] = x;
        travScan[i][1] = y;
        i++;
      }
  return 0;
}
