/* ************************************************************************
 * Copyright (C) 2016-2023 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * ************************************************************************ */

#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <vector>

#include "testing_common.hpp"

/* ============================================================================================ */

using hipblasHerBatchedModel
    = ArgumentModel<e_a_type, e_uplo, e_N, e_alpha, e_incx, e_lda, e_batch_count>;

inline void testname_her_batched(const Arguments& arg, std::string& name)
{
    hipblasHerBatchedModel{}.test_name(arg, name);
}

template <typename T>
void testing_her_batched(const Arguments& arg)
{
    using U      = real_t<T>;
    bool FORTRAN = arg.api == hipblas_client_api::FORTRAN;
    auto hipblasHerBatchedFn
        = FORTRAN ? hipblasHerBatched<T, U, true> : hipblasHerBatched<T, U, false>;

    hipblasFillMode_t uplo        = char2hipblas_fill(arg.uplo);
    int               N           = arg.N;
    int               incx        = arg.incx;
    int               lda         = arg.lda;
    int               batch_count = arg.batch_count;

    size_t A_size = size_t(lda) * N;

    double gpu_time_used, hipblas_error_host, hipblas_error_device;

    U h_alpha = arg.get_alpha<U>();

    hipblasLocalHandle handle(arg);

    // argument sanity check, quick return if input parameters are invalid before allocating invalid
    // memory
    bool invalid_size = N < 0 || lda < N || lda < 1 || !incx || batch_count < 0;
    if(invalid_size || !N || !batch_count)
    {
        hipblasStatus_t actual = hipblasHerBatchedFn(
            handle, uplo, N, nullptr, nullptr, incx, nullptr, lda, batch_count);
        EXPECT_HIPBLAS_STATUS2(
            actual, (invalid_size ? HIPBLAS_STATUS_INVALID_VALUE : HIPBLAS_STATUS_SUCCESS));
        return;
    }

    // Naming: dK is in GPU (device) memory. hK is in CPU (host) memory
    host_batch_vector<T> hA(A_size, 1, batch_count);
    host_batch_vector<T> hA_cpu(A_size, 1, batch_count);
    host_batch_vector<T> hA_host(A_size, 1, batch_count);
    host_batch_vector<T> hA_device(A_size, 1, batch_count);
    host_batch_vector<T> hx(N, incx, batch_count);

    device_batch_vector<T> dA(A_size, 1, batch_count);
    device_batch_vector<T> dx(N, incx, batch_count);
    device_vector<U>       d_alpha(1);

    ASSERT_HIP_SUCCESS(dA.memcheck());
    ASSERT_HIP_SUCCESS(dx.memcheck());

    // Initial Data on CPU
    hipblas_init_vector(hA, arg, hipblas_client_never_set_nan, true);
    hipblas_init_vector(hx, arg, hipblas_client_alpha_sets_nan, false, true);

    hA_cpu.copy_from(hA);
    ASSERT_HIP_SUCCESS(dA.transfer_from(hA));
    ASSERT_HIP_SUCCESS(dx.transfer_from(hx));
    ASSERT_HIP_SUCCESS(hipMemcpy(d_alpha, &h_alpha, sizeof(U), hipMemcpyHostToDevice));

    if(arg.unit_check || arg.norm_check)
    {
        /* =====================================================================
            HIPBLAS
        =================================================================== */
        ASSERT_HIPBLAS_SUCCESS(hipblasSetPointerMode(handle, HIPBLAS_POINTER_MODE_HOST));
        ASSERT_HIPBLAS_SUCCESS(hipblasHerBatchedFn(handle,
                                                   uplo,
                                                   N,
                                                   (U*)&h_alpha,
                                                   dx.ptr_on_device(),
                                                   incx,
                                                   dA.ptr_on_device(),
                                                   lda,
                                                   batch_count));

        ASSERT_HIP_SUCCESS(hA_host.transfer_from(dA));
        ASSERT_HIP_SUCCESS(dA.transfer_from(hA));

        ASSERT_HIPBLAS_SUCCESS(hipblasSetPointerMode(handle, HIPBLAS_POINTER_MODE_DEVICE));
        ASSERT_HIPBLAS_SUCCESS(hipblasHerBatchedFn(handle,
                                                   uplo,
                                                   N,
                                                   d_alpha,
                                                   dx.ptr_on_device(),
                                                   incx,
                                                   dA.ptr_on_device(),
                                                   lda,
                                                   batch_count));

        ASSERT_HIP_SUCCESS(hA_device.transfer_from(dA));

        /* =====================================================================
           CPU BLAS
        =================================================================== */
        for(int b = 0; b < batch_count; b++)
        {
            cblas_her<T>(uplo, N, h_alpha, hx[b], incx, hA_cpu[b], lda);
        }

        // enable unit check, notice unit check is not invasive, but norm check is,
        // unit check and norm check can not be interchanged their order
        if(arg.unit_check)
        {
            unit_check_general<T>(N, N, batch_count, lda, hA_cpu, hA_host);
            unit_check_general<T>(N, N, batch_count, lda, hA_cpu, hA_host);
        }
        if(arg.norm_check)
        {
            hipblas_error_host
                = norm_check_general<T>('F', N, N, lda, hA_cpu, hA_host, batch_count);
            hipblas_error_device
                = norm_check_general<T>('F', N, N, lda, hA_cpu, hA_device, batch_count);
        }
    }

    if(arg.timing)
    {
        ASSERT_HIP_SUCCESS(dA.transfer_from(hA));
        hipStream_t stream;
        ASSERT_HIPBLAS_SUCCESS(hipblasGetStream(handle, &stream));
        ASSERT_HIPBLAS_SUCCESS(hipblasSetPointerMode(handle, HIPBLAS_POINTER_MODE_DEVICE));

        int runs = arg.cold_iters + arg.iters;
        for(int iter = 0; iter < runs; iter++)
        {
            if(iter == arg.cold_iters)
                gpu_time_used = get_time_us_sync(stream);

            ASSERT_HIPBLAS_SUCCESS(hipblasHerBatchedFn(handle,
                                                       uplo,
                                                       N,
                                                       d_alpha,
                                                       dx.ptr_on_device(),
                                                       incx,
                                                       dA.ptr_on_device(),
                                                       lda,
                                                       batch_count));
        }
        gpu_time_used = get_time_us_sync(stream) - gpu_time_used;

        hipblasHerBatchedModel{}.log_args<U>(std::cout,
                                             arg,
                                             gpu_time_used,
                                             her_gflop_count<T>(N),
                                             her_gbyte_count<T>(N),
                                             hipblas_error_host,
                                             hipblas_error_device);
    }
}

template <typename T>
hipblasStatus_t testing_her_batched_ret(const Arguments& arg)
{
    testing_her_batched<T>(arg);
    return HIPBLAS_STATUS_SUCCESS;
}