#include "x86Intrin.hpp"

std::array<voidFunctionType, X86IntrinBinOp::return_intrinsic_count()> X86IntrinBinOp::func_ret = {
  /* sse2_psrl_w */        reinterpret_cast<voidFunctionType>(mm_srl_epi16),
  /* sse2_psrl_d */        reinterpret_cast<voidFunctionType>(mm_srl_epi32),
  /* sse2_psrl_q */        reinterpret_cast<voidFunctionType>(mm_srl_epi64),
  /* avx2_psrl_w */        reinterpret_cast<voidFunctionType>(mm256_srl_epi16),
  /* avx2_psrl_d */        reinterpret_cast<voidFunctionType>(mm256_srl_epi32),
  /* avx2_psrl_q */        reinterpret_cast<voidFunctionType>(mm256_srl_epi64),
  /* sse2_pavg_w */        reinterpret_cast<voidFunctionType>(mm_avg_epu16),
  /* avx2_pavg_b */        reinterpret_cast<voidFunctionType>(mm256_avg_epu8),
  /* avx2_pavg_w */        reinterpret_cast<voidFunctionType>(mm256_avg_epu16),
  /* avx2_pshuf_b */       reinterpret_cast<voidFunctionType>(mm256_shuffle_epi8),
  /* ssse3_pshuf_b_128 */  reinterpret_cast<voidFunctionType>(mm_shuffle_epi8),
  /* mmx_padd_b */         reinterpret_cast<voidFunctionType>(mm_add_pi8),
  /* mmx_padd_w */         reinterpret_cast<voidFunctionType>(mm_add_pi16),
  /* mmx_padd_d */         reinterpret_cast<voidFunctionType>(mm_add_pi32),
  /* mmx_punpckhbw */      reinterpret_cast<voidFunctionType>(mm_unpackhi_pi8),
  /* mmx_punpckhwd */      reinterpret_cast<voidFunctionType>(m_punpckhwd),
  /* mmx_punpckhdq */      reinterpret_cast<voidFunctionType>(mm_unpackhi_pi32),
  /* mmx_punpcklbw */      reinterpret_cast<voidFunctionType>(mm_unpacklo_pi8),
  /* mmx_punpcklwd */      reinterpret_cast<voidFunctionType>(mm_unpacklo_pi16),
  /* mmx_punpckldq */      reinterpret_cast<voidFunctionType>(mm_unpacklo_pi32),
  /* sse2_psrai_w */       reinterpret_cast<voidFunctionType>(mm_sra_epi16),
  /* sse2_psrai_d */       reinterpret_cast<voidFunctionType>(mm_sra_epi32),
  /* avx2_psrai_w */       reinterpret_cast<voidFunctionType>(mm256_sra_epi16),
  /* avx2_psrai_d */       reinterpret_cast<voidFunctionType>(mm256_sra_epi32),
  /* avx512_psrai_w_512 */ reinterpret_cast<voidFunctionType>(mm512_sra_epi16),
  /* avx512_psrai_d_512 */ reinterpret_cast<voidFunctionType>(mm512_sra_epi32),
  /* avx512_psrai_q_128 */ reinterpret_cast<voidFunctionType>(mm_sra_epi64),
  /* avx512_psrai_q_256 */ reinterpret_cast<voidFunctionType>(mm256_sra_epi64),
  /* avx512_psrai_q_512 */ reinterpret_cast<voidFunctionType>(mm512_sra_epi64),
//  /* sse2_psrli_w */       reinterpret_cast<voidFunctionType>(_mm_srl_epi16),
//  /* sse2_psrli_d */       reinterpret_cast<voidFunctionType>(_mm_srl_epi32),
//  /* sse2_psrli_q */       reinterpret_cast<voidFunctionType>(_mm_srl_epi64),
//  /* avx2_psrli_w */       reinterpret_cast<voidFunctionType>(_mm256_srl_epi16),
//  /* avx2_psrli_d */       reinterpret_cast<voidFunctionType>(_mm256_srl_epi32),
//  /* avx2_psrli_q */       reinterpret_cast<voidFunctionType>(_mm256_srl_epi64),
//  /* avx512_psrli_w_512 */ reinterpret_cast<voidFunctionType>(_mm512_srl_epi16),
//  /* avx512_psrli_d_512 */ reinterpret_cast<voidFunctionType>(_mm512_srl_epi32),
//  /* avx512_psrli_q_512 */ reinterpret_cast<voidFunctionType>(_mm512_srl_epi64),
//  /* sse2_pslli_w */       reinterpret_cast<voidFunctionType>(_mm_sll_epi16),
//  /* sse2_pslli_d */       reinterpret_cast<voidFunctionType>(_mm_sll_epi32),
//  /* sse2_pslli_q */       reinterpret_cast<voidFunctionType>(_mm_sll_epi64),
//  /* avx2_pslli_w */       reinterpret_cast<voidFunctionType>(_mm256_sll_epi16),
//  /* avx2_pslli_d */       reinterpret_cast<voidFunctionType>(_mm256_sll_epi32),
//  /* avx2_pslli_q */       reinterpret_cast<voidFunctionType>(_mm256_sll_epi64),
//  /* avx512_pslli_w_512 */ reinterpret_cast<voidFunctionType>(_mm512_sll_epi16),
//  /* avx512_pslli_d_512 */ reinterpret_cast<voidFunctionType>(_mm512_sll_epi32),
//  /* avx512_pslli_q_512 */ reinterpret_cast<voidFunctionType>(_mm512_sll_epi64),
};

std::array<llvm::Intrinsic::ID, X86IntrinBinOp::return_intrinsic_count()> X86IntrinBinOp::intrin_id = {
  llvm::Intrinsic::x86_sse2_psrl_w,
  llvm::Intrinsic::x86_sse2_psrl_d,
  llvm::Intrinsic::x86_sse2_psrl_q,
  llvm::Intrinsic::x86_avx2_psrl_w,
  llvm::Intrinsic::x86_avx2_psrl_d,
  llvm::Intrinsic::x86_avx2_psrl_q,
  llvm::Intrinsic::x86_sse2_pavg_w,
  llvm::Intrinsic::x86_avx2_pavg_b,
  llvm::Intrinsic::x86_avx2_pavg_w,
  llvm::Intrinsic::x86_avx2_pshuf_b,
  llvm::Intrinsic::x86_ssse3_pshuf_b_128,
};
