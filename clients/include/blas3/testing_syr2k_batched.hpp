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

using hipblasSyr2kBatchedModel = ArgumentModel<e_a_type,
                                               e_uplo,
                                               e_transA,
                                               e_N,
                                               e_K,
                                               e_alpha,
                                               e_lda,
                                               e_ldb,
                                               e_beta,
                                               e_ldc,
                                               e_batch_count>;

inline void testname_syr2k_batched(const Arguments& arg, std::string& name)
{
    hipblasSyr2kBatchedModel{}.test_name(arg, name);
}

template <typename T>
void testing_syr2k_batched(const Arguments& arg)
{
    bool FORTRAN = arg.fortran;
    auto hipblasSyr2kBatchedFn
        = FORTRAN ? hipblasSyr2kBatched<T, true> : hipblasSyr2kBatched<T, false>;

    hipblasFillMode_t  uplo        = char2hipblas_fill(arg.uplo);
    hipblasOperation_t transA      = char2hipblas_operation(arg.transA);
    int                N           = arg.N;
    int                K           = arg.K;
    int                lda         = arg.lda;
    int                ldb         = arg.ldb;
    int                ldc         = arg.ldc;
    int                batch_count = arg.batch_count;

    T h_alpha = arg.get_alpha<T>();
    T h_beta  = arg.get_beta<T>();

    // argument sanity check, quick return if input parameters are invalid before allocating invalid
    // memory
    if(N < 0 || K < 0 || ldc < N || (transA == HIPBLAS_OP_N && (lda < N || ldb < N))
       || (transA != HIPBLAS_OP_N && (lda < K || ldb < K)) || batch_count < 0)
    {
        return;
    }
    else if(batch_count == 0)
    {
        return;
    }

    double             gpu_time_used, hipblas_error_host, hipblas_error_device;
    hipblasLocalHandle handle(arg);

    int    K1     = (transA == HIPBLAS_OP_N ? K : N);
    size_t A_size = size_t(lda) * K1;
    size_t B_size = size_t(ldb) * K1;
    size_t C_size = size_t(ldc) * N;

    // Naming: dK is in GPU (device) memory. hK is in CPU (host) memory
    host_batch_vector<T> hA(A_size, 1, batch_count);
    host_batch_vector<T> hB(B_size, 1, batch_count);
    host_batch_vector<T> hC_host(C_size, 1, batch_count);
    host_batch_vector<T> hC_device(C_size, 1, batch_count);
    host_batch_vector<T> hC_gold(C_size, 1, batch_count);

    device_batch_vector<T> dA(A_size, 1, batch_count);
    device_batch_vector<T> dB(B_size, 1, batch_count);
    device_batch_vector<T> dC(C_size, 1, batch_count);
    device_vector<T>       d_alpha(1);
    device_vector<T>       d_beta(1);

    ASSERT_HIP_SUCCESS(dA.memcheck());
    ASSERT_HIP_SUCCESS(dB.memcheck());
    ASSERT_HIP_SUCCESS(dC.memcheck());

    hipblas_init_vector(hA, arg, hipblas_client_never_set_nan, true);
    hipblas_init_vector(hB, arg, hipblas_client_never_set_nan, false, true);
    hipblas_init_vector(hC_host, arg, hipblas_client_never_set_nan);

    hC_device.copy_from(hC_host);
    hC_gold.copy_from(hC_host);

    ASSERT_HIP_SUCCESS(dA.transfer_from(hA));
    ASSERT_HIP_SUCCESS(dB.transfer_from(hB));
    ASSERT_HIP_SUCCESS(dC.transfer_from(hC_host));
    ASSERT_HIP_SUCCESS(hipMemcpy(d_alpha, &h_alpha, sizeof(T), hipMemcpyHostToDevice));
    ASSERT_HIP_SUCCESS(hipMemcpy(d_beta, &h_beta, sizeof(T), hipMemcpyHostToDevice));

    if(arg.unit_check || arg.norm_check)
    {
        /* =====================================================================
            HIPBLAS
        =================================================================== */
        ASSERT_HIPBLAS_SUCCESS(hipblasSetPointerMode(handle, HIPBLAS_POINTER_MODE_HOST));
        ASSERT_HIPBLAS_SUCCESS(hipblasSyr2kBatchedFn(handle,
                                                     uplo,
                                                     transA,
                                                     N,
                                                     K,
                                                     &h_alpha,
                                                     dA.ptr_on_device(),
                                                     lda,
                                                     dB.ptr_on_device(),
                                                     ldb,
                                                     &h_beta,
                                                     dC.ptr_on_device(),
                                                     ldc,
                                                     batch_count));

        ASSERT_HIP_SUCCESS(hC_host.transfer_from(dC));
        ASSERT_HIP_SUCCESS(dC.transfer_from(hC_device));

        ASSERT_HIPBLAS_SUCCESS(hipblasSetPointerMode(handle, HIPBLAS_POINTER_MODE_DEVICE));
        ASSERT_HIPBLAS_SUCCESS(hipblasSyr2kBatchedFn(handle,
                                                     uplo,
                                                     transA,
                                                     N,
                                                     K,
                                                     d_alpha,
                                                     dA.ptr_on_device(),
                                                     lda,
                                                     dB.ptr_on_device(),
                                                     ldb,
                                                     d_beta,
                                                     dC.ptr_on_device(),
                                                     ldc,
                                                     batch_count));

        ASSERT_HIP_SUCCESS(hC_device.transfer_from(dC));

        /* =====================================================================
           CPU BLAS
        =================================================================== */
        for(int b = 0; b < batch_count; b++)
        {
            cblas_syr2k<T>(
                uplo, transA, N, K, h_alpha, hA[b], lda, hB[b], ldb, h_beta, hC_gold[b], ldc);
        }

        // enable unit check, notice unit check is not invasive, but norm check is,
        // unit check and norm check can not be interchanged their order
        if(arg.unit_check)
        {
            unit_check_general<T>(N, N, batch_count, ldc, hC_gold, hC_host);
            unit_check_general<T>(N, N, batch_count, ldc, hC_gold, hC_device);
        }

        if(arg.norm_check)
        {
            hipblas_error_host
                = norm_check_general<T>('F', N, N, ldc, hC_gold, hC_host, batch_count);
            hipblas_error_device
                = norm_check_general<T>('F', N, N, ldc, hC_gold, hC_device, batch_count);
        }
    }

    if(arg.timing)
    {
        hipStream_t stream;
        ASSERT_HIPBLAS_SUCCESS(hipblasGetStream(handle, &stream));
        ASSERT_HIPBLAS_SUCCESS(hipblasSetPointerMode(handle, HIPBLAS_POINTER_MODE_DEVICE));

        int runs = arg.cold_iters + arg.iters;
        for(int iter = 0; iter < runs; iter++)
        {
            if(iter == arg.cold_iters)
                gpu_time_used = get_time_us_sync(stream);

            ASSERT_HIPBLAS_SUCCESS(hipblasSyr2kBatchedFn(handle,
                                                         uplo,
                                                         transA,
                                                         N,
                                                         K,
                                                         d_alpha,
                                                         dA.ptr_on_device(),
                                                         lda,
                                                         dB.ptr_on_device(),
                                                         ldb,
                                                         d_beta,
                                                         dC.ptr_on_device(),
                                                         ldc,
                                                         batch_count));
        }
        gpu_time_used = get_time_us_sync(stream) - gpu_time_used; // in microseconds

        hipblasSyr2kBatchedModel{}.log_args<T>(std::cout,
                                               arg,
                                               gpu_time_used,
                                               syr2k_gflop_count<T>(N, K),
                                               syr2k_gbyte_count<T>(N, K),
                                               hipblas_error_host,
                                               hipblas_error_device);
    }
}

template <typename T>
hipblasStatus_t testing_syr2k_batched_ret(const Arguments& arg)
{
    testing_syr2k_batched<T>(arg);
    return HIPBLAS_STATUS_SUCCESS;
}