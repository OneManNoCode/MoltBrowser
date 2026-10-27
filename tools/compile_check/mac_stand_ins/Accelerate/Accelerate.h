// compile_check STAND-IN for the macOS SDK's Accelerate framework (see
// ../README.md): vecLib's vDSP routines the checked files use, with the
// SDK's types and signatures (vDSP.h).

#ifndef COMPILE_CHECK_STAND_IN_ACCELERATE_H_
#define COMPILE_CHECK_STAND_IN_ACCELERATE_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long vDSP_Length;
typedef long vDSP_Stride;

// Second-order (biquadratic) recursive filter, single and double precision.
extern void vDSP_deq22(const float* __A,
                       vDSP_Stride __IA,
                       const float* __B,
                       float* __C,
                       vDSP_Stride __IC,
                       vDSP_Length __N);
extern void vDSP_deq22D(const double* __A,
                        vDSP_Stride __IA,
                        const double* __B,
                        double* __C,
                        vDSP_Stride __IC,
                        vDSP_Length __N);

#ifdef __cplusplus
}
#endif

#endif  // COMPILE_CHECK_STAND_IN_ACCELERATE_H_
