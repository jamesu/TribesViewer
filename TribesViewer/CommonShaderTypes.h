//-----------------------------------------------------------------------------
// Copyright (c) 2018 James S Urquhart.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//-----------------------------------------------------------------------------

#ifndef CommonShaderTypes_h
#define CommonShaderTypes_h

#include <simd/simd.h>

enum ModelPipelineState
{
   ModelPipeline_DefaultDiffuse,
   ModelPipeline_AdditiveBlend,
   ModelPipeline_SubtractiveBlend,
   ModelPipeline_TranslucentBlend
};

typedef struct
{
   vector_float2 position;
   vector_float4 color;
} TVVertex;

#ifndef __METAL_VERSION__
typedef slm::vec3 packed_float3;
#endif

typedef struct
{
   packed_float3 position;
   packed_float3 normal;
} ModelVertex;

typedef struct
{
   packed_float2 texcoord;
} ModelTexVertex;

typedef struct
{
   packed_float3 position;
   packed_float3 nextPosition;
   packed_float3 normal;
   packed_float4 color;
} LineVertex;

typedef struct
{
   // Per Frame Uniforms
   matrix_float4x4 view_matrix;
   matrix_float4x4 projection_matrix;
   matrix_float4x4 projection_matrix_inverse;
   matrix_float4x4 model_matrix;
   
   vector_uint2 viewport_size;
   
   vector_float4 line_params;
   
   vector_float3 light_pos;
   vector_float4 light_color;
   
   float alpha_test;
} TVUniforms;

typedef enum TVBufferIndices
{
   TVBufferIndexMeshPositions     = 0,
   TVBufferIndexUniforms          = 1,
   TVBufferIndexMeshTexcoords     = 2
} TVBufferIndices;

enum TVBufferTextures
{
  TVTextureDiffuse = 0
};




#endif /* CommonShaderTypes_h */
