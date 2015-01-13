/* Copyright (c) 2011 Khaled Mamou (kmamou at gmail dot com)
 All rights reserved.
 
 
 Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
 
 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 
 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
 
 3. The names of the contributors may not be used to endorse or promote products derived from this software without specific prior written permission.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _CRT_SECURE_NO_WARNINGS

#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#if _OPENMP
#include <omp.h>
#endif // _OPENMP

#include "../public/VHACD.h"
#include "btConvexHullComputer.h"
#include "vhacdMesh.h"
#include "vhacdVolume.h"
#include "vhacdTimer.h"
#include "vhacdVector.h"
#include "vhacdSArray.h"
#include "vhacdICHull.h"
#include "vhacdVHACD.h"

#define MAX(a,b) (((a)>(b))?(a):(b))
#define MIN(a,b) (((a)<(b))?(a):(b))
#define ABS(a) (((a)<0) ? -(a) : (a))
#define ZSGN(a) (((a)<0) ? -1 : (a)>0 ? 1 : 0)

//#define OCL_SOURCE_FROM_FILE
#ifndef OCL_SOURCE_FROM_FILE
const char * oclProgramSource = "\
__kernel void ComputePartialVolumes(__global short4 * voxels,                    \
                                    const    int      numVoxels,                 \
                                    const    float4   plane,                     \
                                    const    float4   minBB,                     \
                                    const    float4   scale,                     \
                                    __local  uint4 *  localPartialVolumes,       \
                                    __global uint4 *  partialVolumes)            \
{                                                                                \
    int localId = get_local_id(0);                                               \
    int groupSize = get_local_size(0);                                           \
    int i0 = get_global_id(0) << 2;                                              \
    float4 voxel;                                                                \
    uint4  v;                                                                    \
    voxel = convert_float4(voxels[i0]);                                          \
    v.s0 = (dot(plane, mad(scale, voxel, minBB)) >= 0.0f) * (i0     < numVoxels);\
    voxel = convert_float4(voxels[i0 + 1]);                                      \
    v.s1 = (dot(plane, mad(scale, voxel, minBB)) >= 0.0f) * (i0 + 1 < numVoxels);\
    voxel = convert_float4(voxels[i0 + 2]);                                      \
    v.s2 = (dot(plane, mad(scale, voxel, minBB)) >= 0.0f) * (i0 + 2 < numVoxels);\
    voxel = convert_float4(voxels[i0 + 3]);                                      \
    v.s3 = (dot(plane, mad(scale, voxel, minBB)) >= 0.0f) * (i0 + 3 < numVoxels);\
    localPartialVolumes[localId] = v;                                            \
    barrier(CLK_LOCAL_MEM_FENCE);                                                \
    for (int i = groupSize >> 1; i > 0; i >>= 1)                                 \
    {                                                                            \
        if (localId < i)                                                         \
        {                                                                        \
            localPartialVolumes[localId] += localPartialVolumes[localId + i];    \
        }                                                                        \
        barrier(CLK_LOCAL_MEM_FENCE);                                            \
    }                                                                            \
    if (localId == 0)                                                            \
    {                                                                            \
        partialVolumes[get_group_id(0)] = localPartialVolumes[0];                \
    }                                                                            \
}                                                                                \
__kernel void ComputePartialSums(__global uint4 * data,                          \
                                 const    int     dataSize,                      \
                                 __local  uint4 * partialSums)                   \
{                                                                                \
    int globalId  = get_global_id(0);                                            \
    int localId   = get_local_id(0);                                             \
    int groupSize = get_local_size(0);                                           \
    int i;                                                                       \
    if (globalId < dataSize)                                                     \
    {                                                                            \
        partialSums[localId] = data[globalId];                                   \
    }                                                                            \
    else                                                                         \
    {                                                                            \
        partialSums[localId] = (0, 0, 0, 0);                                     \
    }                                                                            \
    barrier(CLK_LOCAL_MEM_FENCE);                                                \
    for (i = groupSize >> 1; i > 0; i >>= 1)                                     \
    {                                                                            \
        if (localId < i)                                                         \
        {                                                                        \
            partialSums[localId] += partialSums[localId + i];                    \
        }                                                                        \
        barrier(CLK_LOCAL_MEM_FENCE);                                            \
    }                                                                            \
    if (localId == 0)                                                            \
    {                                                                            \
        data[get_group_id(0)] = partialSums[0];                                  \
    }                                                                            \
}";
#endif //OCL_SOURCE_FROM_FILE

namespace VHACD
{
    IVHACD * CreateVHACD(void)
    {
        return new VHACD();
    }
    bool VHACD::OCLInit(void *        const oclDevice,
                        IUserLogger * const logger)
    {
#ifdef CL_VERSION_1_1
        m_oclDevice  = (cl_device_id *) oclDevice;
        cl_int error;
        m_oclContext = clCreateContext(NULL, 1, m_oclDevice, NULL, NULL, &error);
        if (error != CL_SUCCESS)
        {
            if (logger)
            {
                logger->Log("Couldn't create context\n");
            }
            return false;
        }

#ifdef OCL_SOURCE_FROM_FILE
        std::string cl_files = OPENCL_CL_FILES;
        // read kernal from file
#ifdef _WIN32
        std::replace(cl_files.begin(), cl_files.end(), '/', '\\');
#endif // _WIN32
        
        FILE * program_handle = fopen(cl_files.c_str(), "rb");
        fseek(program_handle, 0, SEEK_END);
        size_t program_size = ftell(program_handle);
        rewind(program_handle);
        char * program_buffer = new char [program_size + 1];
        program_buffer[program_size] = '\0';
        fread(program_buffer, sizeof(char), program_size, program_handle);
        fclose(program_handle);
        // create program
        m_oclProgram = clCreateProgramWithSource(m_oclContext, 1, (const char**)&program_buffer, &program_size, &error);
        delete[] program_buffer;
#else
        size_t program_size = strlen(oclProgramSource);
        m_oclProgram = clCreateProgramWithSource(m_oclContext, 1, (const char**)&oclProgramSource, &program_size, &error);
#endif
        if (error != CL_SUCCESS)
        {
            if (logger)
            {
                logger->Log("Couldn't create program\n");
            }
            return false;
        }

        /* Build program */
        error = clBuildProgram(m_oclProgram, 1, m_oclDevice, "-cl-denorms-are-zero", NULL, NULL);
        if (error != CL_SUCCESS) 
        {
            size_t log_size;
            /* Find size of log and print to std output */
            clGetProgramBuildInfo(m_oclProgram, *m_oclDevice, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
            char * program_log = new char [log_size + 2];
            program_log[log_size    ] = '\n';
            program_log[log_size + 1] = '\0';
            clGetProgramBuildInfo(m_oclProgram, *m_oclDevice, CL_PROGRAM_BUILD_LOG, log_size + 1, program_log, NULL);
            if (logger)
            {
                logger->Log("Couldn't build program\n");
                logger->Log(program_log);
            }
            delete[] program_log;
            return false;
        }

        delete[] m_oclQueue;
        delete[] m_oclKernelComputePartialVolumes;
        delete[] m_oclKernelComputeSum;
        m_oclQueue                       = new cl_command_queue[m_ompNumProcessors];
        m_oclKernelComputePartialVolumes = new cl_kernel[m_ompNumProcessors];
        m_oclKernelComputeSum = new cl_kernel[m_ompNumProcessors];


        const char nameKernelComputePartialVolumes[] = "ComputePartialVolumes";
        const char nameKernelComputeSum[]            = "ComputePartialSums";
        for (int k = 0; k < m_ompNumProcessors; ++k)
        {
            m_oclKernelComputePartialVolumes[k] = clCreateKernel(m_oclProgram, nameKernelComputePartialVolumes, &error);
            if (error != CL_SUCCESS)
            {
                if (logger)
                {
                    logger->Log("Couldn't create kernel\n");
                }
                return false;
            }
            m_oclKernelComputeSum[k] = clCreateKernel(m_oclProgram, nameKernelComputeSum, &error);
            if (error != CL_SUCCESS)
            {
                if (logger)
                {
                    logger->Log("Couldn't create kernel\n");
                }
                return false;
            }
        }

        error = clGetKernelWorkGroupInfo(m_oclKernelComputePartialVolumes[0],
                                         *m_oclDevice,
                                         CL_KERNEL_WORK_GROUP_SIZE,
                                         sizeof(size_t),
                                         &m_oclWorkGroupSize,
                                         NULL);
        size_t workGroupSize = 0;
        error = clGetKernelWorkGroupInfo(m_oclKernelComputeSum[0],
                                         *m_oclDevice,
                                         CL_KERNEL_WORK_GROUP_SIZE,
                                         sizeof(size_t),
                                         &workGroupSize,
                                         NULL);
        if (error != CL_SUCCESS)
        {
            if (logger)
            {
                logger->Log("Couldn't query work group info\n");
            }
            return false;
        }

        if (workGroupSize < m_oclWorkGroupSize)
        {
            m_oclWorkGroupSize = workGroupSize;
        }

        for (int k = 0; k < m_ompNumProcessors; ++k)
        {
            m_oclQueue[k] = clCreateCommandQueue(m_oclContext, *m_oclDevice, 0 /*CL_QUEUE_PROFILING_ENABLE*/, &error);
            if (error != CL_SUCCESS)
            {
                if (logger)
                {
                    logger->Log("Couldn't create queue\n");
                }
                return false;
            }
        }
        return true;
#else //CL_VERSION_1_1
        return false;
#endif //CL_VERSION_1_1
    }
    bool VHACD::OCLRelease(IUserLogger * const logger)
    {
#ifdef CL_VERSION_1_1
        cl_int error;
        if (m_oclKernelComputePartialVolumes)
        {
            for (int k = 0; k < m_ompNumProcessors; ++k)
            {
                error = clReleaseKernel(m_oclKernelComputePartialVolumes[k]);
                if (error != CL_SUCCESS)
                {
                    if (logger)
                    {
                        logger->Log("Couldn't release kernal\n");
                    }
                    return false;
                }
            }
            delete[] m_oclKernelComputePartialVolumes;
        }
        if (m_oclKernelComputeSum)
        {
            for (int k = 0; k < m_ompNumProcessors; ++k)
            {
                error = clReleaseKernel(m_oclKernelComputeSum[k]);
                if (error != CL_SUCCESS)
                {
                    if (logger)
                    {
                        logger->Log("Couldn't release kernal\n");
                    }
                    return false;
                }
            }
            delete[] m_oclKernelComputeSum;
        }
        if (m_oclQueue)
        {
            for (int k = 0; k < m_ompNumProcessors; ++k)
            {
                error = clReleaseCommandQueue(m_oclQueue[k]);
                if (error != CL_SUCCESS)
                {
                    if (logger)
                    {
                        logger->Log("Couldn't release queue\n");
                    }
                    return false;
                }
            }
            delete[] m_oclQueue;
        }
        error = clReleaseProgram(m_oclProgram);
        if (error != CL_SUCCESS)
        {
            if (logger)
            {
                logger->Log("Couldn't release program\n");
            }
            return false;
        }
        error = clReleaseContext(m_oclContext);
        if (error != CL_SUCCESS)
        {
            if (logger)
            {
                logger->Log("Couldn't release context\n");
            }
            return false;
        }

        return true;
#else  //CL_VERSION_1_1
        return false;
#endif //CL_VERSION_1_1
    }
    void VHACD::ComputePrimitiveSet(const Parameters &  params)
    {
        if (GetCancel())
        {
            return;
        }
        m_timer.Tic();

        m_stage = "Compute primitive set";
        m_operation = "Convert volume to pset";

        std::ostringstream msg;
        if (params.m_logger)
        {
            msg << "+ " << m_stage << std::endl;
            params.m_logger->Log(msg.str().c_str());
        }

        Update(0.0, 0.0, params);
        if (params.m_mode == 0)
        {
            VoxelSet * vset = new VoxelSet;
            m_volume->Convert(*vset);
            m_pset = vset;
        }
        else
        {
            TetrahedronSet * tset = new TetrahedronSet;
            m_volume->Convert(*tset);
            m_pset = tset;
        }
        
        delete m_volume;
        m_volume = 0;

        if (params.m_logger)
        {
            msg.str("");
            msg << "\t # primitives               " <<m_pset->GetNPrimitives() << std::endl;
            msg << "\t # inside surface           " <<m_pset->GetNPrimitivesInsideSurf() << std::endl;
            msg << "\t # on surface               " <<m_pset->GetNPrimitivesOnSurf() << std::endl;
            params.m_logger->Log(msg.str().c_str());
        }

        m_overallProgress = 15.0;
        Update(100.0, 100.0, params);
        m_timer.Toc();
        if (params.m_logger)
        {
            msg.str("");
            msg << "\t time " << m_timer.GetElapsedTime() / 1000.0 << "s" << std::endl;
            params.m_logger->Log(msg.str().c_str());
        }
    }
    bool VHACD::Compute(const double * const points,
                        const unsigned int  stridePoints,
                        const unsigned int  nPoints,
                        const int   * const triangles,
                        const unsigned int  strideTriangles,
                        const unsigned int  nTriangles,
                        const Parameters &  params)
    {
        return ComputeACD(points, stridePoints, nPoints, triangles, strideTriangles, nTriangles, params);
    }
    bool VHACD::Compute(const float * const points,
                        const unsigned int  stridePoints,
                        const unsigned int  nPoints,
                        const int   * const triangles,
                        const unsigned int  strideTriangles,
                        const unsigned int  nTriangles,
                        const Parameters &  params)
    {
        return ComputeACD(points, stridePoints, nPoints, triangles, strideTriangles, nTriangles, params);
    }
    double ComputePreferredCuttingDirection(const PrimitiveSet * const tset, Vec3<double> & dir)
    {
        double ex = tset->GetEigenValue(AXIS_X);
        double ey = tset->GetEigenValue(AXIS_Y);
        double ez = tset->GetEigenValue(AXIS_Z);
        double vx = (ey - ez)*(ey - ez);
        double vy = (ex - ez)*(ex - ez);
        double vz = (ex - ey)*(ex - ey);
        if (vx < vy && vx < vz)
        {
            double e = ey*ey + ez*ez;
            dir[0] = 1.0;
            dir[1] = 0.0;
            dir[2] = 0.0;
            return (e == 0.0) ? 0.0 : 1.0 - vx / e;
        }
        else if (vy < vx && vy < vz)
        {
            double e = ex*ex + ez*ez;
            dir[0] = 0.0;
            dir[1] = 1.0;
            dir[2] = 0.0;
            return (e == 0.0) ? 0.0 : 1.0 - vy / e;
        }
        else
        {
            double e = ex*ex + ey*ey;
            dir[0] = 0.0;
            dir[1] = 0.0;
            dir[2] = 1.0;
            return (e == 0.0) ? 0.0 : 1.0 - vz / e;
        }
    }
    void ComputeAxesAlignedClippingPlanes(const VoxelSet & vset, 
                                          const short      downsampling, 
                                          SArray<Plane>  & planes)
    {
        const Vec3<short> minV = vset.GetMinBBVoxels();
        const Vec3<short> maxV = vset.GetMaxBBVoxels();
        Vec3<double> pt;
        Plane      plane;
        const short i0 = minV[0];
        const short i1 = maxV[0];
        plane.m_a    = 1.0;
        plane.m_b    = 0.0;
        plane.m_c    = 0.0;
        plane.m_axis = AXIS_X;
        for(short i = i0; i <= i1; i += downsampling)
        {
            pt            = vset.GetPoint(Vec3<double>(i + 0.5, 0.0, 0.0));
            plane.m_d     = -pt[0];
            plane.m_index = i;
            planes.PushBack(plane);
        }
        const short j0 = minV[1];
        const short j1 = maxV[1];
        plane.m_a    = 0.0;
        plane.m_b    = 1.0;
        plane.m_c    = 0.0;
        plane.m_axis = AXIS_Y;
        for(short j = j0; j <= j1; j += downsampling)
        {
            pt            = vset.GetPoint(Vec3<double>(0.0, j + 0.5, 0.0));
            plane.m_d     = -pt[1];
            plane.m_index = j;
            planes.PushBack(plane);
        }
        const short k0 = minV[2];
        const short k1 = maxV[2];
        plane.m_a    = 0.0;
        plane.m_b    = 0.0;
        plane.m_c    = 1.0;
        plane.m_axis = AXIS_Z;
        for(short k = k0; k <= k1; k += downsampling)
        {
            pt            = vset.GetPoint(Vec3<double>(0.0, 0.0, k + 0.5));
            plane.m_d     = -pt[2];
            plane.m_index = k;
            planes.PushBack(plane);
        }
    }
    void ComputeAxesAlignedClippingPlanes(const TetrahedronSet & tset,
                                          const short            downsampling,
                                          SArray< Plane >      & planes)
    {
        const Vec3<double> minV  = tset.GetMinBB();
        const Vec3<double> maxV  = tset.GetMaxBB();
        const double       scale = tset.GetSacle();
        const short        i0    = 0;
        const short        j0    = 0;
        const short        k0    = 0;
        const short        i1    = (short)((maxV[0] - minV[0]) / scale + 0.5);
        const short        j1    = (short)((maxV[1] - minV[1]) / scale + 0.5);
        const short        k1    = (short)((maxV[2] - minV[2]) / scale + 0.5);

        Plane plane;
        plane.m_a = 1.0;
        plane.m_b = 0.0;
        plane.m_c = 0.0;
        plane.m_axis = AXIS_X;
        for (short i = i0; i <= i1; i += downsampling)
        {
            double x = minV[0] + scale * i;
            plane.m_d = -x;
            plane.m_index = i;
            planes.PushBack(plane);
        }
        plane.m_a = 0.0;
        plane.m_b = 1.0;
        plane.m_c = 0.0;
        plane.m_axis = AXIS_Y;
        for (short j = j0; j <= j1; j += downsampling)
        {
            double y = minV[1] + scale * j;
            plane.m_d = -y;
            plane.m_index = j;
            planes.PushBack(plane);
        }
        plane.m_a = 0.0;
        plane.m_b = 0.0;
        plane.m_c = 1.0;
        plane.m_axis = AXIS_Z;
        for (short k = k0; k <= k1; k += downsampling)
        {
            double z = minV[2] + scale * k;
            plane.m_d = -z;
            plane.m_index = k;
            planes.PushBack(plane);
        }
    }
    void RefineAxesAlignedClippingPlanes(const VoxelSet & vset,
                                         const Plane    & bestPlane,
                                         const short      downsampling,
                                         SArray<Plane>  & planes)
    {
        const Vec3<short> minV = vset.GetMinBBVoxels();
        const Vec3<short> maxV = vset.GetMaxBBVoxels();
        Vec3<double> pt;
        Plane      plane;

        if (bestPlane.m_axis == AXIS_X)
        {
            const short i0 = MAX(minV[0], bestPlane.m_index - downsampling);
            const short i1 = MIN(maxV[0], bestPlane.m_index + downsampling);
            plane.m_a = 1.0;
            plane.m_b = 0.0;
            plane.m_c = 0.0;
            plane.m_axis = AXIS_X;
            for (short i = i0; i <= i1; ++i)
            {
                pt = vset.GetPoint(Vec3<double>(i + 0.5, 0.0, 0.0));
                plane.m_d = -pt[0];
                plane.m_index = i;
                planes.PushBack(plane);
            }
        }
        else if (bestPlane.m_axis == AXIS_Y)
        {
            const short j0 = MAX(minV[1], bestPlane.m_index - downsampling);
            const short j1 = MIN(maxV[1], bestPlane.m_index + downsampling);
            plane.m_a = 0.0;
            plane.m_b = 1.0;
            plane.m_c = 0.0;
            plane.m_axis = AXIS_Y;
            for (short j = j0; j <= j1; ++j)
            {
                pt = vset.GetPoint(Vec3<double>(0.0, j + 0.5, 0.0));
                plane.m_d = -pt[1];
                plane.m_index = j;
                planes.PushBack(plane);
            }
        }
        else
        {
            const short k0 = MAX(minV[2], bestPlane.m_index - downsampling);
            const short k1 = MIN(maxV[2], bestPlane.m_index + downsampling);
            plane.m_a = 0.0;
            plane.m_b = 0.0;
            plane.m_c = 1.0;
            plane.m_axis = AXIS_Z;
            for (short k = k0; k <= k1; ++k)
            {
                pt = vset.GetPoint(Vec3<double>(0.0, 0.0, k + 0.5));
                plane.m_d = -pt[2];
                plane.m_index = k;
                planes.PushBack(plane);
            }
        }
    }
    void RefineAxesAlignedClippingPlanes(const TetrahedronSet & tset,
                                         const  Plane         & bestPlane,
                                         const  short           downsampling,
                                         SArray< Plane >      & planes)
    {
        const Vec3<double> minV  = tset.GetMinBB();
        const Vec3<double> maxV  = tset.GetMaxBB();
        const double       scale = tset.GetSacle();
        Plane              plane;

        if (bestPlane.m_axis == AXIS_X)
        {
            const short i0 = MAX(0, bestPlane.m_index - downsampling);
            const short i1 = (short)MIN((maxV[0] - minV[0]) / scale + 0.5, bestPlane.m_index + downsampling);
            plane.m_a = 1.0;
            plane.m_b = 0.0;
            plane.m_c = 0.0;
            plane.m_axis = AXIS_X;
            for (short i = i0; i <= i1; ++i)
            {
                double x = minV[0] + scale * i;
                plane.m_d = -x;
                plane.m_index = i;
                planes.PushBack(plane);
            }
        }
        else if (bestPlane.m_axis == AXIS_Y)
        {
            const short j0 = MAX(0, bestPlane.m_index - downsampling);
            const short j1 = (short)MIN((maxV[1] - minV[1]) / scale + 0.5, bestPlane.m_index + downsampling);
            plane.m_a = 0.0;
            plane.m_b = 1.0;
            plane.m_c = 0.0;
            plane.m_axis = AXIS_Y;
            for (short j = j0; j <= j1; ++j)
            {
                double y = minV[1] + scale * j;
                plane.m_d = -y;
                plane.m_index = j;
                planes.PushBack(plane);
            }
        }
        else
        {
            const short k0 = MAX(0, bestPlane.m_index - downsampling);
            const short k1 = (short)MIN((maxV[2] - minV[2]) / scale + 0.5, bestPlane.m_index + downsampling);
            plane.m_a = 0.0;
            plane.m_b = 0.0;
            plane.m_c = 1.0;
            plane.m_axis = AXIS_Z;
            for (short k = k0; k <= k1; ++k)
            {
                double z = minV[2] + scale * k;
                plane.m_d = -z;
                plane.m_index = k;
                planes.PushBack(plane);
            }
        }
    }
    
//#define DEBUG_TEMP
    void VHACD::ComputeBestClippingPlane(const PrimitiveSet    * inputPSet,
                                         const double            volume0,
                                         const double            volume,
                                         const SArray< Plane > & planes,
                                         const Vec3<double>    & preferredCuttingDirection,
                                         const double            w,
                                         const double            alpha,
                                         const double            beta,
                                         const int               convexhullDownsampling,
                                         const double            progress0,
                                         const double            progress1,
                                         Plane                 & bestPlane,
                                         double                & minConcavity,
                                         double                & minBalance,
                                         double                & minSymmetry,
                                         const Parameters      & params)
    {
        if (GetCancel())
        {
            return;
        }
        char msg[1024];
        int  iBest = -1;
        int  nPlanes = (int) static_cast<int>(planes.Size());
        double minTotal = minConcavity + minBalance + minSymmetry;
        bool cancel = false;
        int done = 0;

        SArray< Vec3<double> > * chPts = new SArray< Vec3<double> >[2 * m_ompNumProcessors];
        Mesh                   * chs   = new Mesh[2 * m_ompNumProcessors];

        PrimitiveSet * onSurfacePSet = inputPSet->Create();
        inputPSet->SelectOnSurface(onSurfacePSet);

        PrimitiveSet ** psets = 0;
        if (!params.m_convexhullApproximation)
        {
            psets = new PrimitiveSet *[2 * m_ompNumProcessors];
            for (int i = 0; i < 2 * m_ompNumProcessors; ++i)
            {
                psets[i] = inputPSet->Create();
            }
        }

        size_t nPrimitives = inputPSet->GetNPrimitives();
        bool oclAcceleration = (nPrimitives > OCL_MIN_NUM_PRIMITIVES && params.m_oclAcceleration && params.m_mode == 0) ? true : false;

#ifdef CL_VERSION_1_1
        // allocate OpenCL data structures
        cl_mem voxels;
        cl_mem * partialVolumes = 0;
        size_t globalSize       = 0;
        size_t nWorkGroups      = 0;
        double unitVolume       = 0.0;
        if (oclAcceleration)
        {
            VoxelSet * vset          = (VoxelSet *)inputPSet;
            const Vec3<double> minBB = vset->GetMinBB();
            const float fMinBB[4]    = { (float)minBB[0], (float)minBB[1], (float)minBB[2], 1.0f };
            const float fSclae[4]    = { (float)vset->GetScale(), (float)vset->GetScale(), (float)vset->GetScale(), 0.0f };
            const int   nVoxels      = (int)nPrimitives;
            unitVolume               = vset->GetUnitVolume();
            nWorkGroups              = (nPrimitives + 4 * m_oclWorkGroupSize - 1) / (4 * m_oclWorkGroupSize);
            globalSize               = nWorkGroups * m_oclWorkGroupSize;
            cl_int error;
            voxels = clCreateBuffer(m_oclContext,
                                    CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                    sizeof(Voxel) * nPrimitives,
                                    vset->GetVoxels(),
                                    &error);
            if (error != CL_SUCCESS)
            {
                if (params.m_logger)
                {
                    params.m_logger->Log("Couldn't create buffer\n");
                }
                SetCancel(true);
            }

            partialVolumes = new cl_mem[m_ompNumProcessors];
            for (int i = 0; i < m_ompNumProcessors; ++i)
            {
                partialVolumes[i] = clCreateBuffer(m_oclContext,
                                                   CL_MEM_WRITE_ONLY,
                                                   sizeof(unsigned int) * 4 * nWorkGroups,
                                                   NULL,
                                                   &error);
                if (error != CL_SUCCESS)
                {
                    if (params.m_logger)
                    {
                        params.m_logger->Log("Couldn't create buffer\n");
                    }
                    SetCancel(true);
                    break;
                }
                error  = clSetKernelArg(m_oclKernelComputePartialVolumes[i], 0, sizeof(cl_mem)                               , &voxels);
                error |= clSetKernelArg(m_oclKernelComputePartialVolumes[i], 1, sizeof(unsigned int)                         , &nVoxels);
                error |= clSetKernelArg(m_oclKernelComputePartialVolumes[i], 3, sizeof(float) * 4                            , fMinBB);
                error |= clSetKernelArg(m_oclKernelComputePartialVolumes[i], 4, sizeof(float) * 4                            , &fSclae);
                error |= clSetKernelArg(m_oclKernelComputePartialVolumes[i], 5, sizeof(unsigned int) * 4 * m_oclWorkGroupSize, NULL);
                error |= clSetKernelArg(m_oclKernelComputePartialVolumes[i], 6, sizeof(cl_mem)                               , &(partialVolumes[i]));
                error |= clSetKernelArg(m_oclKernelComputeSum[i]           , 0, sizeof(cl_mem)                               , &(partialVolumes[i]));
                error |= clSetKernelArg(m_oclKernelComputeSum[i]           , 2, sizeof(unsigned int) * 4 * m_oclWorkGroupSize, NULL);
                if (error != CL_SUCCESS)
                {
                    if (params.m_logger)
                    {
                        params.m_logger->Log("Couldn't kernel atguments \n");
                    }
                    SetCancel(true);
                }
            }
        }
#else  // CL_VERSION_1_1
        oclAcceleration = false;
#endif // CL_VERSION_1_1

#if USE_THREAD == 1 && _OPENMP
        #pragma omp parallel for
#endif
        for (int x = 0; x < nPlanes; ++x)
        {
            int threadID = 0;
#if USE_THREAD == 1 && _OPENMP
            threadID = omp_get_thread_num();
            #pragma omp flush (cancel)
#endif
            if (!cancel)
            {
                //Update progress
                if (GetCancel())
                {
                    cancel = true;
#if USE_THREAD == 1 && _OPENMP
                    #pragma omp flush (cancel)
#endif
                }
                Plane plane = planes[x];

#ifdef DEBUG_TEMP
                Timer timerComputeCost;
                timerComputeCost.Tic();
#endif // DEBUG_TEMP

                if (oclAcceleration)
                {
#ifdef CL_VERSION_1_1
                    const float fPlane[4] = { (float)plane.m_a, (float)plane.m_b, (float)plane.m_c, (float)plane.m_d };
                    cl_int      error     = clSetKernelArg(m_oclKernelComputePartialVolumes[threadID], 2, sizeof(float) * 4, fPlane);
                    if (error != CL_SUCCESS)
                    {
                        if (params.m_logger)
                        {
                            params.m_logger->Log("Couldn't kernel atguments \n");
                        }
                        SetCancel(true);
                    }

                    error = clEnqueueNDRangeKernel(m_oclQueue[threadID],
                                                   m_oclKernelComputePartialVolumes[threadID],
                                                   1,
                                                   NULL,
                                                   &globalSize,
                                                   &m_oclWorkGroupSize,
                                                   0,
                                                   NULL,
                                                   NULL);
                    if (error != CL_SUCCESS)
                    {
                        if (params.m_logger)
                        {
                            params.m_logger->Log("Couldn't run kernel \n");
                        }
                        SetCancel(true);
                    }
                    int nValues = (int)nWorkGroups;
                    while (nValues > 1)
                    {
                        error = clSetKernelArg(m_oclKernelComputeSum[threadID], 1, sizeof(int), &nValues);
                        if (error != CL_SUCCESS)
                        {
                            if (params.m_logger)
                            {
                                params.m_logger->Log("Couldn't kernel atguments \n");
                            }
                            SetCancel(true);
                        }
                        size_t nWorkGroups = (nValues + m_oclWorkGroupSize - 1) / m_oclWorkGroupSize;
                        size_t globalSize  = nWorkGroups * m_oclWorkGroupSize;
                        error = clEnqueueNDRangeKernel(m_oclQueue[threadID],
                                                       m_oclKernelComputeSum[threadID],
                                                       1,
                                                       NULL,
                                                       &globalSize,
                                                       &m_oclWorkGroupSize,
                                                       0,
                                                       NULL,
                                                       NULL);
                        if (error != CL_SUCCESS)
                        {
                            if (params.m_logger)
                            {
                                params.m_logger->Log("Couldn't run kernel \n");
                            }
                            SetCancel(true);
                        }
                        nValues = (int)nWorkGroups;
                    }
#endif // CL_VERSION_1_1
                }

#ifdef DEBUG_TEMP
                Timer timerConvexHullVolumes;
                timerConvexHullVolumes.Tic();
#endif // DEBUG_TEMP

                Mesh                   & leftCH = chs[threadID];
                Mesh                   & rightCH = chs[threadID + m_ompNumProcessors];
                rightCH.ResizePoints(0);
                leftCH.ResizePoints(0);
                rightCH.ResizeTriangles(0);
                leftCH.ResizeTriangles(0);

                // compute convex-hulls
                if (params.m_convexhullApproximation)
                {
                    SArray< Vec3<double> > & leftCHPts = chPts[threadID];
                    SArray< Vec3<double> > & rightCHPts = chPts[threadID + m_ompNumProcessors];
                    rightCHPts.Resize(0);
                    leftCHPts.Resize(0);
                    onSurfacePSet->Intersect(plane, &rightCHPts, &leftCHPts);
                    inputPSet->GetConvexHull().Clip(plane, rightCHPts, leftCHPts);
                    rightCH.ComputeConvexHull((double *)rightCHPts.Data(), rightCHPts.Size());
                    leftCH.ComputeConvexHull((double *)leftCHPts.Data(), leftCHPts.Size());
                }
                else
                {
                    PrimitiveSet * const right = psets[threadID];
                    PrimitiveSet * const left = psets[threadID + m_ompNumProcessors];
                    onSurfacePSet->Clip(plane, right, left);
                    right->ComputeConvexHull(rightCH, convexhullDownsampling);
                    left->ComputeConvexHull(leftCH, convexhullDownsampling);
                }

                double volumeLeftCH = leftCH.ComputeVolume();
                double volumeRightCH = rightCH.ComputeVolume();
#ifdef DEBUG_TEMP
                timerConvexHullVolumes.Toc();
#endif // DEBUG_TEMP


                // compute clipped volumes
                double volumeLeft  = 0.0;
                double volumeRight = 0.0;


                if (oclAcceleration)
                {
#ifdef CL_VERSION_1_1
                    unsigned int volumes[4];
                    cl_int error = clEnqueueReadBuffer(m_oclQueue[threadID],
                                                       partialVolumes[threadID],
                                                       CL_TRUE,
                                                       0,
                                                       sizeof(unsigned int) * 4,
                                                       volumes,
                                                       0,
                                                       NULL,
                                                       NULL);
                    size_t nPrimitivesRight = volumes[0] + volumes[1] + volumes[2] + volumes[3];
                    size_t nPrimitivesLeft  = nPrimitives - nPrimitivesRight;
                    volumeRight = nPrimitivesRight * unitVolume;
                    volumeLeft  = nPrimitivesLeft * unitVolume;
                    if (error != CL_SUCCESS)
                    {
                        if (params.m_logger)
                        {
                            params.m_logger->Log("Couldn't read buffer \n");
                        }
                        SetCancel(true);
                    }
#endif // CL_VERSION_1_1
                }
                else
                {
                    inputPSet->ComputeClippedVolumes(plane, volumeRight, volumeLeft);
                }

                // compute cost
                double concavity = fabs(volumeLeftCH + volumeRightCH - volume) / volume0;
                double balance   = alpha * pow(pow(volumeLeft - volumeRight, 2.0), 0.5) / volume0;
                double d         = w * (preferredCuttingDirection[0] * plane.m_a + preferredCuttingDirection[1] * plane.m_b + preferredCuttingDirection[2] * plane.m_c);
                double symmetry  = beta * d;
                double total     = concavity + balance + symmetry;

#ifdef DEBUG_TEMP
                timerComputeCost.Toc();
                printf_s("ConvexHullVolumes[%i/%i] = %f\n", x, nPlanes, timerConvexHullVolumes.GetElapsedTime());
                printf_s("Cost[%i/%i] = %f\n", x, nPlanes, timerComputeCost.GetElapsedTime());
#endif // DEBUG_TEMP

#if USE_THREAD == 1 && _OPENMP
                #pragma omp critical
#endif
                {
                    if (total <  minTotal || (total == minTotal && x < iBest))
                    {
                        /*
                        if (params.m_logger)
                        {
                            sprintf(msg, "\t\t\t Plane %04i T=%2.3f C=%2.3f B=%2.3f S=%2.3f D=%1.6f W=%1.6f [%1.1f, %1.1f, %1.1f](%1.1f, %1.1f, %1.1f, %3.3f) \n",
                                x, total, concavity, balance, symmetry, d, w,
                                preferredCuttingDirection[0], preferredCuttingDirection[1], preferredCuttingDirection[2],
                                plane.m_a, plane.m_b, plane.m_c, plane.m_d);
                            params.m_logger->Log(msg);
                        }
                        */
                        bestPlane    = plane;
                        minTotal     = total;
                        minConcavity = concavity;
                        iBest        = x;
                        minBalance   = balance;
                        minSymmetry  = symmetry;
                    }
                    ++done;
                    if (!(done & 127)) // reduce update frequency
                    {
                        double progress = done * (progress1 - progress0) / nPlanes + progress0;
                        Update(m_stageProgress, progress, params);
                    }
                }
            }
        }
#ifdef CL_VERSION_1_1
        if (oclAcceleration)
        {
            clReleaseMemObject(voxels);
            for (int i = 0; i < m_ompNumProcessors; ++i)
            {
                clReleaseMemObject(partialVolumes[i]);
            }
            delete[] partialVolumes;
        }
#endif // CL_VERSION_1_1

        if (psets)
        {
            for (int i = 0; i < 2 * m_ompNumProcessors; ++i)
            {
                delete psets[i];
            }
            delete[] psets;
        }
        delete onSurfacePSet;
        delete[] chPts;
        delete[] chs;
        if (params.m_logger)
        {
            sprintf(msg, "\n\t\t\t Best  %04i T=%2.3f C=%2.3f B=%2.3f S=%2.3f (%1.1f, %1.1f, %1.1f, %3.3f)\n\n", iBest, minTotal, minConcavity, minBalance, minSymmetry, bestPlane.m_a, bestPlane.m_b, bestPlane.m_c, bestPlane.m_d);
            params.m_logger->Log(msg);
        }
    }
    void VHACD::ComputeACD(const Parameters &  params)
    {
        if (GetCancel())
        {
            return;
        }
        m_timer.Tic();

        m_stage = "Approximate Convex Decomposition";
        m_stageProgress = 0.0;
        std::ostringstream msg;
        if (params.m_logger)
        {
            msg << "+ " << m_stage << std::endl;
            params.m_logger->Log(msg.str().c_str());
        }

        SArray< PrimitiveSet * > parts;
        SArray< PrimitiveSet * > inputParts;
        SArray< PrimitiveSet * > temp;
        inputParts.PushBack(m_pset);
        m_pset = 0;
        SArray< Plane > planes;
        SArray< Plane > planesRef;
        int sub = 0;
        bool firstIteration = true;
        m_volume0 = 1.0;
        double concavity0 = 1.0;
        while (sub++ < params.m_depth && inputParts.Size() > 0 && !m_cancel)
        {
            msg.str("");
            msg << "Subdivision level " << sub;
            m_operation = msg.str();

            if (params.m_logger)
            {
                msg.str("");
                msg << "\t Subdivision level " << sub << std::endl;
                params.m_logger->Log(msg.str().c_str());
            }

            double maxConcavity = 0.0;
            const size_t nInputParts = inputParts.Size();
            Update(m_stageProgress, 0.0, params);
            for (size_t p = 0; p < nInputParts && !m_cancel; ++p)
            {
                const double progress0 = p          * 100.0 / nInputParts;
                const double progress1 = (p + 0.75) * 100.0 / nInputParts;
                const double progress2 = (p + 1.00) * 100.0 / nInputParts;

                Update(m_stageProgress, progress0, params);
                

                PrimitiveSet * pset = inputParts[p];
                inputParts[p] = 0;
                double volume = pset->ComputeVolume();
                pset->ComputeBB();
                pset->ComputePrincipalAxes();
                if (params.m_pca)
                {
                    pset->AlignToPrincipalAxes();
                }

                pset->ComputeConvexHull(pset->GetConvexHull(), 1);
                double volumeCH = fabs(pset->GetConvexHull().ComputeVolume());

                if (firstIteration)
                {
                    m_volume0 = volumeCH;
                }

                double error = 1.01 * pset->ComputeMaxVolumeError() / m_volume0;
                double concavity = fabs(volumeCH - volume) / m_volume0;

                if (firstIteration)
                {
                    concavity0 = concavity;
                    firstIteration = false;
                }


                if (params.m_logger)
                {
                    msg.str("");
                    msg << "\t -> Part[" << p
                        << "] C = " << concavity
                        << ", E = " << error
                        << ", VS = " << pset->GetNPrimitivesOnSurf()
                        << ", VI = " << pset->GetNPrimitivesInsideSurf()
                        << std::endl;
                    params.m_logger->Log(msg.str().c_str());
                }

                if (concavity > params.m_concavity && concavity > error)
                {
                    Vec3<double> preferredCuttingDirection;
                    double w = ComputePreferredCuttingDirection(pset, preferredCuttingDirection);
                    planes.Resize(0);
                    if (params.m_mode == 0)
                    {
                        VoxelSet * vset = (VoxelSet *) pset;
                        ComputeAxesAlignedClippingPlanes(*vset, params.m_planeDownsampling, planes);
                    }
                    else
                    {
                        TetrahedronSet * tset = (TetrahedronSet *) pset;
                        ComputeAxesAlignedClippingPlanes(*tset, params.m_planeDownsampling, planes);
                    }

                    if (params.m_logger)
                    {
                        msg.str("");
                        msg << "\t\t [Regular sampling] Number of clipping planes " << planes.Size() << std::endl;
                        params.m_logger->Log(msg.str().c_str());
                    }

                    Plane bestPlane;
                    double minConcavity = 1.0;
                    double minBalance = 1.0;
                    double minSymmetry = 1.0;

                    ComputeBestClippingPlane(pset,
                                             m_volume0,
                                             volume,
                                             planes,
                                             preferredCuttingDirection,
                                             w,
                                             concavity * params.m_alpha,
                                             concavity * params.m_beta,
                                             params.m_convexhullDownsampling,
                                             progress0,
                                             progress1,
                                             bestPlane,
                                             minConcavity,
                                             minBalance,
                                             minSymmetry,
                                             params);
                    if (!m_cancel && (params.m_planeDownsampling > 1 || params.m_convexhullDownsampling > 1))
                    {
                        planesRef.Resize(0);

                        if (params.m_mode == 0)
                        {
                            VoxelSet * vset = (VoxelSet *) pset;
                            RefineAxesAlignedClippingPlanes(*vset, bestPlane, params.m_planeDownsampling, planesRef);
                        }
                        else
                        {
                            TetrahedronSet * tset = (TetrahedronSet *) pset;
                            RefineAxesAlignedClippingPlanes(*tset, bestPlane, params.m_planeDownsampling, planesRef);
                        }

                        if (params.m_logger)
                        {
                            msg.str("");
                            msg << "\t\t [Refining] Number of clipping planes " << planesRef.Size() << std::endl;
                            params.m_logger->Log(msg.str().c_str());
                        }
                        minConcavity = 1.0;
                        minBalance   = 1.0;
                        minSymmetry  = 1.0;
                        ComputeBestClippingPlane(pset,
                                                 m_volume0,
                                                 volume,
                                                 planesRef,
                                                 preferredCuttingDirection,
                                                 w,
                                                 concavity * params.m_alpha,
                                                 concavity * params.m_beta,
                                                 1,                 // convexhullDownsampling = 1
                                                 progress1,
                                                 progress2,
                                                 bestPlane,
                                                 minConcavity,
                                                 minBalance,
                                                 minSymmetry,
                                                 params);
                    }
                    if (GetCancel())
                    {
                        delete pset; // clean up
                        break;
                    }
                    else
                    {
                        if (maxConcavity < minConcavity)
                        {
                            maxConcavity = minConcavity;
                        }
                        PrimitiveSet  * bestLeft  = pset->Create();
                        PrimitiveSet  * bestRight = pset->Create();
                        temp.PushBack(bestLeft);
                        temp.PushBack(bestRight);
                        pset->Clip(bestPlane, bestRight, bestLeft);
                        if (params.m_pca)
                        {
                            bestRight->RevertAlignToPrincipalAxes();
                            bestLeft->RevertAlignToPrincipalAxes();
                        }
                        delete pset;
                    }
                }
                else
                {
                    if (params.m_pca)
                    {
                        pset->RevertAlignToPrincipalAxes();
                    }
                    parts.PushBack(pset);
                }
            }

            Update(95.0*(1.0 - maxConcavity) / (1.0 - params.m_concavity), 100.0, params);
            if (GetCancel())
            {
                const size_t nTempParts = temp.Size();
                for (size_t p = 0; p < nTempParts; ++p)
                {
                    delete temp[p];
                }
                temp.Resize(0);
            }
            else
            {
                inputParts = temp;
                temp.Resize(0);
            }
        }
        const size_t nInputParts = inputParts.Size();
        for (size_t p = 0; p < nInputParts; ++p)
        {
            parts.PushBack(inputParts[p]);
        }

        if (GetCancel())
        {
            const size_t nParts = parts.Size();
            for (size_t p = 0; p < nParts; ++p)
            {
                delete parts[p];
            }
            return;
        }

        m_overallProgress = 90.0;
        Update(m_stageProgress, 100.0, params);


        msg.str("");
        msg << "Generate convex-hulls";
        m_operation = msg.str();
        size_t nConvexHulls = parts.Size();
        if (params.m_logger)
        {
            msg.str("");
            msg << "+ Generate " << nConvexHulls << " convex-hulls " << std::endl;
            params.m_logger->Log(msg.str().c_str());
        }

        Update(m_stageProgress, 0.0, params);
        m_convexHulls.Resize(0);
        for (size_t p = 0; p < nConvexHulls && !m_cancel; ++p)
        {
            Update(m_stageProgress, p * 100.0 / nConvexHulls, params);
            m_convexHulls.PushBack(new Mesh);
            parts[p]->ComputeConvexHull(*m_convexHulls[p], 1);
            size_t nv = m_convexHulls[p]->GetNPoints();
            double x, y, z;
            for (size_t i = 0; i < nv; ++i)
            {
                Vec3<double> & pt = m_convexHulls[p]->GetPoint(i);
                x = pt[0];
                y = pt[1];
                z = pt[2];
                pt[0] = m_rot[0][0] * x + m_rot[0][1] * y + m_rot[0][2] * z + m_barycenter[0];
                pt[1] = m_rot[1][0] * x + m_rot[1][1] * y + m_rot[1][2] * z + m_barycenter[1];
                pt[2] = m_rot[2][0] * x + m_rot[2][1] * y + m_rot[2][2] * z + m_barycenter[2];
            }
        }

        const size_t nParts = parts.Size();
        for (size_t p = 0; p < nParts; ++p)
        {
            delete parts[p];
            parts[p] = 0;
        }
        parts.Resize(0);

        if (GetCancel())
        {
            const size_t nConvexHulls = m_convexHulls.Size();
            for (size_t p = 0; p < nConvexHulls; ++p)
            {
                delete m_convexHulls[p];
            }
            m_convexHulls.Clear();
            return;
        }

        m_overallProgress = 95.0;
        Update(100.0, 100.0, params);
        m_timer.Toc();
        if (params.m_logger)
        {
            msg.str("");
            msg << "\t time " << m_timer.GetElapsedTime() / 1000.0 << "s" << std::endl;
            params.m_logger->Log(msg.str().c_str());
        }
    }
    void AddPoints(const Mesh * const mesh, SArray< Vec3<double> > & pts)
    {
        const int n = (int) mesh->GetNPoints();
        for(int i = 0; i < n; ++i)
        {
            pts.PushBack(mesh->GetPoint(i));
        }
    }
    void ComputeConvexHull(const Mesh * const     ch1, 
                           const Mesh * const     ch2,
                           SArray< Vec3<double> > & pts,
                           Mesh * const           combinedCH)
    {
        pts.Resize(0);
        AddPoints(ch1, pts);
        AddPoints(ch2, pts);

        btConvexHullComputer ch;
        ch.compute((double *)pts.Data(), 3 * sizeof(double), (int)pts.Size(), -1.0, -1.0);
        combinedCH->ResizePoints   (0);
        combinedCH->ResizeTriangles(0);
        for (int v = 0; v < ch.vertices.size(); v++)
        {
            combinedCH->AddPoint(Vec3<double>(ch.vertices[v].getX(), ch.vertices[v].getY(), ch.vertices[v].getZ()));
        }
        const int nt = ch.faces.size();
        for (int t = 0; t < nt; ++t)
        {
            const btConvexHullComputer::Edge * sourceEdge = &(ch.edges[ch.faces[t]]);
            int a = sourceEdge->getSourceVertex();
            int b = sourceEdge->getTargetVertex();
            const btConvexHullComputer::Edge * edge = sourceEdge->getNextEdgeOfFace();
            int c = edge->getTargetVertex();
            while (c != a)
            {
                combinedCH->AddTriangle(Vec3<int>(a, b, c));
                edge = edge->getNextEdgeOfFace();
                b = c;
                c = edge->getTargetVertex();
            }
        }
    }
    void VHACD::MergeConvexHulls(const Parameters &  params)
    {
        if (GetCancel())
        {
            return;
        }
        m_timer.Tic();

        m_stage             = "Merge Convex Hulls";

        std::ostringstream msg;
        if (params.m_logger)
        {
            msg << "+ " << m_stage << std::endl;
            params.m_logger->Log(msg.str().c_str());
        }

        size_t nConvexHulls = m_convexHulls.Size();
        int iteration = 0;
        if (nConvexHulls > 1 && !m_cancel)
        {
            const size_t nConvexHulls0 = nConvexHulls;
            const double threshold = params.m_gamma * m_volume0;
            SArray< Vec3<double> > pts;
            Mesh combinedCH;
            bool iterate = true;

            while (iterate && !m_cancel)
            {
                msg.str("");
                msg << "Iteration " << iteration++;
                m_operation = msg.str();

                size_t bestp1;
                size_t bestp2;
                double bestCost = m_volume0;
                for (size_t p1 = 0; (p1 < nConvexHulls0 - 1) && (!m_cancel); ++p1)
                {
                    Update(iteration * 100.0 / nConvexHulls0, p1 * 100.0 / (nConvexHulls0 - 1), params);

                    if (m_convexHulls[p1] && !m_cancel)
                    {
                        double volume1 = m_convexHulls[p1]->ComputeVolume();
                        size_t p2 = p1 + 1;
                        while (p2 < nConvexHulls0)
                        {
                            if (p1 != p2 && m_convexHulls[p2] && !m_cancel)
                            {
                                double volume2 = m_convexHulls[p2]->ComputeVolume();

                                ComputeConvexHull(m_convexHulls[p1], m_convexHulls[p2], pts, &combinedCH);

                                double combinedVolume = combinedCH.ComputeVolume();
                                double cost           = combinedVolume - volume1 - volume2;
                                if (cost < bestCost)
                                {
                                    bestCost       = cost;
                                    bestp1         = p1;
                                    bestp2         = p2;
                                    if (params.m_logger)
                                    {
                                        msg.str("");
                                        msg << "\t\t Cost (" << p1 << ", " << p2 << ") " << cost / m_volume0 << std::endl;
                                        params.m_logger->Log(msg.str().c_str());
                                    }
                                }
                            }
                            ++p2;
                        }
                    }
                }
                if (bestCost < threshold && !m_cancel)
                {
                    if (params.m_logger)
                    {
                        msg.str("");
                        msg << "\t\t Merging (" << bestp1 << ", " << bestp2 << ") " << bestCost / m_volume0 << std::endl << std::endl;
                        params.m_logger->Log(msg.str().c_str());
                    }
                    Mesh * cch = new Mesh;
                    ComputeConvexHull(m_convexHulls[bestp1], m_convexHulls[bestp2], pts, cch);
                    delete m_convexHulls[bestp1];
                    delete m_convexHulls[bestp2];
                    m_convexHulls[bestp2] = 0;
                    m_convexHulls[bestp1] = cch;
                    iterate             = true;
                    --nConvexHulls;
                }
                else
                {
                    iterate             = false;
                }
            }
            if (!m_cancel)
            {
                SArray<Mesh *> temp;
                temp.Allocate(nConvexHulls);
                for (size_t p1 = 0; p1 < nConvexHulls0; ++p1)
                {
                    if (m_convexHulls[p1])
                    {
                        temp.PushBack(m_convexHulls[p1]);
                    }
                }
                m_convexHulls = temp;
            }
        }
        m_overallProgress = 99.0;
        Update(100.0, 100.0, params);
        m_timer.Toc();
        if (params.m_logger)
        {
            msg.str("");
            msg << "\t time " << m_timer.GetElapsedTime() / 1000.0 << "s" << std::endl;
            params.m_logger->Log(msg.str().c_str());
        }
    }
    void SimplifyConvexHull(Mesh * const ch,
                            const size_t nvertices,
                            const double minVolume)
    {
        if (nvertices <= 4)
        {
            return;
        }
        ICHull icHull;
        icHull.AddPoints(ch->GetPointsBuffer(), ch->GetNPoints());
        icHull.Process((unsigned int)nvertices, minVolume);
        TMMesh & mesh = icHull.GetMesh();
        const size_t nT = mesh.GetNTriangles();
        const size_t nV = mesh.GetNVertices();
        ch->ResizePoints(nV);
        ch->ResizeTriangles(nT);
        mesh.GetIFS(ch->GetPointsBuffer(), ch->GetTrianglesBuffer());
    }
    void VHACD::SimplifyConvexHulls(const Parameters &  params)
    {
        if (m_cancel || params.m_maxNumVerticesPerCH < 4)
        {
            return;
        }
        m_timer.Tic();

        m_stage     = "Simplify convex-hulls";
        m_operation = "Simplify convex-hulls";

        std::ostringstream msg;
        const size_t nConvexHulls = m_convexHulls.Size();
        if (params.m_logger)
        {
            msg << "+ Simplify " << nConvexHulls << " convex-hulls "  << std::endl;
            params.m_logger->Log(msg.str().c_str());
        }

        Update(0.0, 0.0, params);
        for (size_t i = 0; i < nConvexHulls && !m_cancel; ++i)
        {
            if (params.m_logger)
            {
                msg.str("");
                msg << "\t\t Simplify CH[" << std::setfill('0') << std::setw(5) << i << "] " << m_convexHulls[i]->GetNPoints() << " V, " << m_convexHulls[i]->GetNTriangles() << " T" << std::endl;
                params.m_logger->Log(msg.str().c_str());
            }
            SimplifyConvexHull(m_convexHulls[i], params.m_maxNumVerticesPerCH, m_volume0*params.m_minVolumePerCH);
        }

        m_overallProgress = 100.0;
        Update(100.0, 100.0, params);
        m_timer.Toc();
        if (params.m_logger)
        {
            msg.str("");
            msg << "\t time " << m_timer.GetElapsedTime() / 1000.0 << "s" << std::endl;
            params.m_logger->Log(msg.str().c_str());
        }
    }
}

