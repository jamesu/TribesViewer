//-----------------------------------------------------------------------------
// Copyright (c) 2018-2024 James S Urquhart.
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

#include <stdint.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <strings.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>
#include <unordered_map>
#include <slm/slmath.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_wgpu.h"

#include "CommonShaderTypes.h"
#include "CommonData.h"

#include "lineShader.wgsl.h"
#include "modelShader.wgsl.h"
#include "terrainShader.wgsl.h"

#include "RendererHelper.h"


extern "C"
{
#if defined(__APPLE__) || defined(WGPU_NATIVE)
#include "webgpu.h"
#else
#include <webgpu/webgpu.h>
#endif
}

static inline size_t AlignSize(const size_t size, const uint16_t alignment)
{
   return (size + (alignment - 1)) & ~(alignment - 1);
}

struct CommonUniformStruct
{
   slm::mat4 projMat;
   slm::mat4 viewMat;
   slm::mat4 modelMat;
   slm::vec4 params1;
   slm::vec4 params2;
   slm::vec4 lightPos;
   slm::vec4 lightColor;
   
   slm::vec4 squareTexCoords[8*2];
};

struct BaseProgramInfo
{
   CommonUniformStruct uniforms;
   
   BaseProgramInfo() { memset(&uniforms, '\0', sizeof(CommonUniformStruct)); }
};

struct ModelProgramInfo : public BaseProgramInfo
{
   WGPURenderPipeline pipelines[ModelPipeline_Count];
   
   ModelProgramInfo() { memset(pipelines, '\0', sizeof(pipelines)); }
   
   void reset()
   {
      for (int i=0; i<ModelPipeline_Count; i++)
      {
         if (pipelines[i])
            wgpuRenderPipelineRelease(pipelines[i]);
         pipelines[i] = NULL;
      }
   }
};

struct LineProgramInfo : public BaseProgramInfo
{
   WGPURenderPipeline pipeline;
   
   LineProgramInfo() { pipeline = NULL; }
   
   void reset()
   {
      if (pipeline)
         wgpuRenderPipelineRelease(pipeline);
      pipeline = NULL;
   }
};

struct TerrainProgramInfo : public BaseProgramInfo
{
   WGPURenderPipeline pipelines[2];
   
   TerrainProgramInfo() { memset(this, '\0', sizeof(TerrainProgramInfo)); }
   
   void reset()
   {
      if (pipelines[0])
         wgpuRenderPipelineRelease(pipelines[0]);
      if (pipelines[1])
         wgpuRenderPipelineRelease(pipelines[1]);
      pipelines[0] = NULL;
      pipelines[1] = NULL;
   }
};

struct SDLState
{
   enum GpuInitState
   {
      INIT_NONE,
      INIT_SURFACE,
      GOT_SURFACE,
      INIT_ADAPTER,
      GOT_ADAPTER,
      INIT_DEVICE,
      GOT_DEVICE,
      INIT_SWAPCHAIN
   };
   
   struct BufferRef
   {
      WGPUBuffer buffer;
      size_t offset;
      size_t size;
   };
   
   struct BufferAlloc
   {
      WGPUBuffer buffer;
      uint32_t flags;
      size_t head;
      size_t size;
   };
   
   struct FrameModel
   {
      BufferRef vertOffset;
      BufferRef texVertOffset;
      BufferRef indexOffset;
      
      uint32_t numVerts;
      uint32_t numTexVerts;
      uint32_t numInds;
      
      // local
      ModelVertex* vertData;
      ModelTexVertex* texVertData;
      uint16_t* indexData;
      
      // uploaded to gpu this frame?
      bool inFrame;
   };
   
   struct TexInfo
   {
      WGPUTexture texture;
      WGPUTextureView textureView;
      WGPUBindGroup texBindGroup;
      uint32_t dims[3];
   };
   
   std::vector<FrameModel> models;
   std::vector<TexInfo> textures;
   
   // Resource state
   std::unordered_map<std::string, WGPUShaderModule> shaders;
   std::vector<BufferAlloc> buffers;
   
   WGPUSampler modelCommonSampler;
   WGPUSampler modelCommonLinearSampler;
   WGPUSampler modelCommonLinearClampSampler;
   WGPUBindGroupLayout commonUniformLayout;
   WGPUBindGroupLayout commonTextureLayout;
   WGPUBindGroupLayout terrainTextureLayout;
   WGPUBindGroupLayout terrainSamplersLayout;
   WGPUBindGroup commonUniformGroup;
   BufferRef commonUniformBuffer;
   
   LineProgramInfo lineProgram;
   ModelProgramInfo modelProgram;
   TerrainProgramInfo terrainProgram;
   
   //
   
   slm::mat4 projectionMatrix;
   slm::mat4 modelMatrix;
   slm::mat4 viewMatrix;
   
   slm::vec4 lightColor;
   slm::vec3 lightPos;
   slm::vec2 viewportSize;
   
   SDL_Window *window;
   SDL_Renderer *renderer;
   
   // Core state
   WGPUAdapter gpuAdapter;
   WGPUDevice gpuDevice;
   WGPUInstance gpuInstance;
   WGPUQueue gpuQueue;
   WGPUSurface gpuSurface;
   WGPUSurfaceConfiguration gpuSurfaceConfig;
   WGPUBackendType gpuBackendType;
   
   // Swapchain
   WGPUSurfaceTexture gpuSurfaceTexture;
   WGPUTextureView gpuSurfaceTextureView;
   WGPUTexture depthTexture;
   WGPUTextureView depthTextureView;
   WGPUTextureFormat depthStencilFormat;
   
   // Frame state
   WGPURenderPassEncoder renderEncoder;
   WGPUCommandEncoder commandEncoder;
   BaseProgramInfo* currentProgram;
   WGPURenderPipeline currentPipeline;
   
   // Samplers
   
   struct TerrainGPUResource
   {
      uint32_t mMatListTexID;
      uint32_t mHeightMapTexID;
      uint32_t mGridMapTexID;
      uint32_t mLightMapTexID;
      WGPUBindGroup mBindGroup;
   };
   
   std::vector<TerrainGPUResource> terrainResources;
   
   int32_t backingSize[2];
   float backingScale;
   
   GpuInitState gpuInitState; // surface ->
   
   // Util funcs (mainly webgpu related)
   
   SDLState();
   
   bool initWGPUSurface();
   void requestWGPUAdapter();
   void requestWGPUDevice();
   bool initWGPUSwapchain();
   
   void resetWGPUState();
   void resetWGPUSwapChain();
   
   bool loadShaderModule(const char* name, const char* code);
   BufferRef allocBuffer(size_t size, uint32_t flags, uint16_t alignment);
   void resetBufferAllocs();
   
   void beginRenderPass(bool secondary);
   void endRenderPass();
   
   WGPUBindGroup makeSimpleTextureBG(WGPUTextureView tex, WGPUSampler sampler);
   WGPUBindGroup makeTerrainTextureBG(WGPUTextureView squareMatTex, WGPUTextureView heightmapTex, WGPUTextureView mapTex, WGPUTextureView lmTex, WGPUSampler samplerPixel, WGPUSampler samplerLinear);
   WGPURenderPassDescriptor createRenderPass(bool secondary);
};


SDLState smState;


static LineProgramInfo buildLineProgram()
{
   LineProgramInfo ret;
   
   // Create the pipeline layout
   WGPUPipelineLayoutDescriptor pipelineLayoutDesc = {};
   pipelineLayoutDesc.label = "Pipeline Layout";
   pipelineLayoutDesc.bindGroupLayoutCount = 1;
   WGPUBindGroupLayout bindGroupLayouts[1] = {smState.commonUniformLayout};
   pipelineLayoutDesc.bindGroupLayouts = bindGroupLayouts;
   
   WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(smState.gpuDevice, &pipelineLayoutDesc);
   
   // Vertex buffer layout for stream 0 (LineVertex)
   WGPUVertexAttribute vertexAttributes[4];
   
   // Position attribute
   vertexAttributes[0] = {};
   vertexAttributes[0].format = WGPUVertexFormat_Float32x3;
   vertexAttributes[0].offset = offsetof(_LineVert, pos);
   vertexAttributes[0].shaderLocation = 0; // Corresponding to `position` in the shader
   
   // Next position attribute
   vertexAttributes[1] = {};
   vertexAttributes[1].format = WGPUVertexFormat_Float32x3;
   vertexAttributes[1].offset = offsetof(_LineVert, nextPos);
   vertexAttributes[1].shaderLocation = 1; // Corresponding to `nextPosition` in the shader
   
   // Normal attribute
   vertexAttributes[2] = {};
   vertexAttributes[2].format = WGPUVertexFormat_Float32x3;
   vertexAttributes[2].offset = offsetof(_LineVert, normal);
   vertexAttributes[2].shaderLocation = 2; // Corresponding to `normal` in the shader
   
   // Color attribute
   vertexAttributes[3] = {};
   vertexAttributes[3].format = WGPUVertexFormat_Float32x4;
   vertexAttributes[3].offset = offsetof(_LineVert, color);
   vertexAttributes[3].shaderLocation = 3; // Corresponding to `color` in the shader
   
   // Define the vertex buffer layout
   WGPUVertexBufferLayout vertexBufferLayout = {};
   vertexBufferLayout.arrayStride = sizeof(_LineVert);
   vertexBufferLayout.stepMode = WGPUVertexStepMode_Vertex; // Per-vertex data
   vertexBufferLayout.attributeCount = 4;
   vertexBufferLayout.attributes = vertexAttributes;
   
   // Vertex state configuration
   WGPUVertexState vertexState = {};
   vertexState.module = smState.shaders["lineShader"];  // Loaded shader module
   vertexState.entryPoint = "mainVert";          // Entry point of the vertex shader
   vertexState.bufferCount = 1;
   vertexState.buffers = &vertexBufferLayout;
   
   // Create the bind group layout for group 0 (if needed)
   WGPUBindGroupLayoutEntry bindGroupLayoutEntry0 = {};
   bindGroupLayoutEntry0.binding = 0;
   bindGroupLayoutEntry0.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
   bindGroupLayoutEntry0.buffer.type = WGPUBufferBindingType_Uniform;
   bindGroupLayoutEntry0.buffer.minBindingSize = sizeof(CommonUniformStruct);
   
   WGPUBindGroupLayoutDescriptor bindGroupLayoutDesc0 = {};
   bindGroupLayoutDesc0.label = "Uniform Bind Group Layout";
   bindGroupLayoutDesc0.entryCount = 1;
   bindGroupLayoutDesc0.entries = &bindGroupLayoutEntry0;
   
   WGPUBindGroupLayout bindGroupLayout0 = wgpuDeviceCreateBindGroupLayout(smState.gpuDevice, &bindGroupLayoutDesc0);
   
   // Fragment state
   WGPUFragmentState fragmentState = {};
   fragmentState.module = smState.shaders["lineShader"]; // Loaded fragment shader module
   fragmentState.entryPoint = "mainFrag";           // Entry point of the fragment shader
   fragmentState.targetCount = 1;
   
   WGPUColorTargetState colorTargetState = {};
   colorTargetState.format = WGPUTextureFormat_BGRA8Unorm; // Output texture format
   colorTargetState.blend = NULL;                          // No blending
   colorTargetState.writeMask = WGPUColorWriteMask_All;    // Write all color channels
   
   fragmentState.targets = &colorTargetState;
   
   // Define the primitive state (we assume you are drawing lines, so topology is LineList)
   WGPUPrimitiveState primitiveState = {};
   primitiveState.topology = WGPUPrimitiveTopology_TriangleList;
   primitiveState.stripIndexFormat = WGPUIndexFormat_Undefined;  // Non-indexed drawing
   primitiveState.frontFace = WGPUFrontFace_CW;                  // Not relevant since there's no culling
   primitiveState.cullMode = WGPUCullMode_None;                  // No culling for lines
   
   // Multisample state
   WGPUMultisampleState multisampleState = {};
   multisampleState.count = 1;
   multisampleState.mask = ~0;
   multisampleState.alphaToCoverageEnabled = false;
   
   // Depth stencil state
   // (NOTE: we need this even if we aren't using it)
   WGPUDepthStencilState depthStencilState = {};
   depthStencilState.format = WGPUTextureFormat_Depth32Float;      // Depth format
   depthStencilState.depthWriteEnabled = false;                    // Enable depth writing
   depthStencilState.depthCompare = WGPUCompareFunction_Always;
   depthStencilState.stencilFront.compare = WGPUCompareFunction_Always;
   depthStencilState.stencilFront.failOp = WGPUStencilOperation_Keep;
   depthStencilState.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
   depthStencilState.stencilFront.passOp = WGPUStencilOperation_Keep;
   depthStencilState.stencilBack = depthStencilState.stencilFront; // Same as front
   depthStencilState.stencilReadMask = 0xFFFFFFFF;
   depthStencilState.stencilWriteMask = 0xFFFFFFFF;
   depthStencilState.depthBias = 0;                               // No depth bias
   depthStencilState.depthBiasSlopeScale = 0.0f;
   depthStencilState.depthBiasClamp = 0.0f;
   
   // Create the render pipeline descriptor
   WGPURenderPipelineDescriptor pipelineDesc = {};
   pipelineDesc.label = "Render Pipeline";
   pipelineDesc.layout = pipelineLayout;
   pipelineDesc.vertex = vertexState;
   pipelineDesc.primitive = primitiveState;
   pipelineDesc.fragment = &fragmentState;
   pipelineDesc.depthStencil = &depthStencilState;
   pipelineDesc.multisample = multisampleState;
   
   // Finally, create the pipeline
   ret.pipeline = wgpuDeviceCreateRenderPipeline(smState.gpuDevice, &pipelineDesc);
   
   return ret;
}


static TerrainProgramInfo buildTerrainProgram()
{
   TerrainProgramInfo ret;
   
   // Create the pipeline layout
   WGPUPipelineLayoutDescriptor pipelineLayoutDesc = {};
   pipelineLayoutDesc.label = "Pipeline Layout";
   pipelineLayoutDesc.bindGroupLayoutCount = 2;
   WGPUBindGroupLayout bindGroupLayouts[2] = {smState.commonUniformLayout, smState.terrainTextureLayout};
   pipelineLayoutDesc.bindGroupLayouts = bindGroupLayouts;
   
   WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(smState.gpuDevice, &pipelineLayoutDesc);
   
   // Define the vertex buffer layout
   WGPUVertexBufferLayout vertexBufferLayout = {};
   vertexBufferLayout.arrayStride = 0;
   vertexBufferLayout.stepMode = WGPUVertexStepMode_VertexBufferNotUsed;
   vertexBufferLayout.attributeCount = 0;
   vertexBufferLayout.attributes = NULL;
   
   // Vertex state configuration
   WGPUVertexState vertexState = {};
   vertexState.module = smState.shaders["terrainShader"];  // Loaded shader module
   vertexState.entryPoint = "vertMain";          // Entry point of the vertex shader
   vertexState.bufferCount = 0;
   vertexState.buffers = NULL;
   
   // Fragment state
   WGPUFragmentState fragmentState = {};
   fragmentState.module = smState.shaders["terrainShader"]; // Loaded fragment shader module
   fragmentState.entryPoint = "fragMain";           // Entry point of the fragment shader
   fragmentState.targetCount = 1;
   
   WGPUColorTargetState colorTargetState = {};
   colorTargetState.format = WGPUTextureFormat_BGRA8Unorm; // Output texture format
   colorTargetState.blend = NULL;                          // No blending
   colorTargetState.writeMask = WGPUColorWriteMask_All;    // Write all color channels
   
   fragmentState.targets = &colorTargetState;
   
   // Define the primitive state (we assume you are drawing lines, so topology is LineList)
   WGPUPrimitiveState primitiveState = {};
   primitiveState.topology = WGPUPrimitiveTopology_TriangleList;
   primitiveState.stripIndexFormat = WGPUIndexFormat_Undefined;  // Non-indexed drawing
   primitiveState.frontFace = WGPUFrontFace_CW;                  // Not relevant since there's no culling
   primitiveState.cullMode = WGPUCullMode_None;                  // No culling for lines
   
   // Multisample state
   WGPUMultisampleState multisampleState = {};
   multisampleState.count = 1;
   multisampleState.mask = ~0;
   multisampleState.alphaToCoverageEnabled = false;
   
   // Depth stencil state
   // (NOTE: we need this even if we aren't using it)
   WGPUDepthStencilState depthStencilState = {};
   depthStencilState.format = WGPUTextureFormat_Depth32Float;      // Depth format
   depthStencilState.depthWriteEnabled = true;                    // Enable depth writing
   depthStencilState.depthCompare = WGPUCompareFunction_Less;     // Use less-than
   depthStencilState.stencilFront.compare = WGPUCompareFunction_Always;
   depthStencilState.stencilFront.failOp = WGPUStencilOperation_Keep;
   depthStencilState.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
   depthStencilState.stencilFront.passOp = WGPUStencilOperation_Keep;
   depthStencilState.stencilBack = depthStencilState.stencilFront; // Same as front
   depthStencilState.stencilReadMask = 0xFFFFFFFF;
   depthStencilState.stencilWriteMask = 0xFFFFFFFF;
   depthStencilState.depthBias = 0;                               // No depth bias
   depthStencilState.depthBiasSlopeScale = 0.0f;
   depthStencilState.depthBiasClamp = 0.0f;
   
   // Create the render pipeline descriptor
   WGPURenderPipelineDescriptor pipelineDesc = {};
   pipelineDesc.label = "Render Pipeline";
   pipelineDesc.layout = pipelineLayout;
   pipelineDesc.vertex = vertexState;
   pipelineDesc.primitive = primitiveState;
   pipelineDesc.fragment = &fragmentState;
   pipelineDesc.depthStencil = &depthStencilState;
   pipelineDesc.multisample = multisampleState;
   
   // Finally, create the pipeline
   ret.pipelines[0] = wgpuDeviceCreateRenderPipeline(smState.gpuDevice, &pipelineDesc);
   
   return ret;
}

ModelProgramInfo buildModelProgram()
{
   ModelProgramInfo ret;
   
   // Create the pipeline layout
   WGPUPipelineLayoutDescriptor pipelineLayoutDesc = {};
   pipelineLayoutDesc.label = "Pipeline Layout";
   pipelineLayoutDesc.bindGroupLayoutCount = 2;
   WGPUBindGroupLayout bindGroupLayouts[2] = {smState.commonUniformLayout, smState.commonTextureLayout};
   pipelineLayoutDesc.bindGroupLayouts = bindGroupLayouts;
   
   WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(smState.gpuDevice, &pipelineLayoutDesc);
   
   auto buildPipelineForState = [&pipelineLayout](ModelPipelineState state){
      // Vertex buffer layout for stream 0 (ModelVertex)
      WGPUVertexAttribute vertexAttributes0[2];
      vertexAttributes0[0] = {};
      vertexAttributes0[0].format = WGPUVertexFormat_Float32x3;
      vertexAttributes0[0].offset = offsetof(ModelVertex, position);
      vertexAttributes0[0].shaderLocation = 0;
      
      vertexAttributes0[1] = {};
      vertexAttributes0[1].format = WGPUVertexFormat_Float32x3;
      vertexAttributes0[1].offset = offsetof(ModelVertex, normal);
      vertexAttributes0[1].shaderLocation = 1;
      
      WGPUVertexBufferLayout vertexBufferLayout0 = {};
      vertexBufferLayout0.arrayStride = sizeof(ModelVertex);
      vertexBufferLayout0.stepMode = WGPUVertexStepMode_Vertex; // Per-vertex data
      vertexBufferLayout0.attributeCount = 2;
      vertexBufferLayout0.attributes = vertexAttributes0;
      
      // Vertex buffer layout for stream 1 (ModelTexVertex)
      WGPUVertexAttribute vertexAttributes1[1];
      vertexAttributes1[0] = {};
      vertexAttributes1[0].format = WGPUVertexFormat_Float32x2;
      vertexAttributes1[0].offset = offsetof(ModelTexVertex, texcoord);
      vertexAttributes1[0].shaderLocation = 2;
      
      WGPUVertexBufferLayout vertexBufferLayout1 = {};
      vertexBufferLayout1.arrayStride = sizeof(ModelTexVertex);
      vertexBufferLayout1.stepMode = WGPUVertexStepMode_Vertex; // Per-vertex data
      vertexBufferLayout1.attributeCount = 1;
      vertexBufferLayout1.attributes = vertexAttributes1;
      
      // Vertex state configuration
      WGPUVertexState vertexState = {};
      vertexState.module = smState.shaders["modelShader"];  // Loaded shader module
      vertexState.entryPoint = "mainVert";          // Entry point of the vertex shader
      vertexState.bufferCount = 2;
      WGPUVertexBufferLayout vertexLayouts[2] = {vertexBufferLayout0, vertexBufferLayout1};
      vertexState.buffers = vertexLayouts;
      
      // Create the pipeline layout
      WGPUPipelineLayoutDescriptor pipelineLayoutDesc = {};
      pipelineLayoutDesc.label = "Pipeline Layout";
      pipelineLayoutDesc.bindGroupLayoutCount = 2;
      WGPUBindGroupLayout bindGroupLayouts[2] = {smState.commonUniformLayout, smState.commonTextureLayout};
      pipelineLayoutDesc.bindGroupLayouts = bindGroupLayouts;
      
      WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(smState.gpuDevice, &pipelineLayoutDesc);
      
      // Fragment state
      WGPUFragmentState fragmentState = {};
      fragmentState.module = smState.shaders["modelShader"]; // Loaded fragment shader module
      fragmentState.entryPoint = "mainFrag";         // Entry point of the fragment shader
      fragmentState.targetCount = 1;
      
      WGPUColorTargetState colorTargetState = {};
      colorTargetState.format = WGPUTextureFormat_BGRA8Unorm; // Output texture format
      
      WGPUBlendState blendState = {};
      colorTargetState.blend = NULL;
      
      switch (state)
      {
         case ModelPipeline_AdditiveBlend:
            blendState.color.operation = WGPUBlendOperation_Add;
            blendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
            blendState.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            
            blendState.alpha.operation = WGPUBlendOperation_Add;
            blendState.alpha.srcFactor = WGPUBlendFactor_One;
            blendState.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            colorTargetState.blend = &blendState;
            break;
         case ModelPipeline_SubtractiveBlend:
            blendState.color.operation = WGPUBlendOperation_Add;
            blendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
            blendState.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            
            blendState.alpha.operation = WGPUBlendOperation_Add;
            blendState.alpha.srcFactor = WGPUBlendFactor_One;
            blendState.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            colorTargetState.blend = &blendState;
            break;
         case ModelPipeline_TranslucentBlend:
            blendState.color.operation = WGPUBlendOperation_Add;
            blendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
            blendState.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            
            blendState.alpha.operation = WGPUBlendOperation_Add;
            blendState.alpha.srcFactor = WGPUBlendFactor_One;
            blendState.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            colorTargetState.blend = &blendState;
            break;
            
         case ModelPipeline_DefaultDiffuse:
         default:
            break;
      };
      
      colorTargetState.writeMask = WGPUColorWriteMask_All;
      
      fragmentState.targets = &colorTargetState;
      
      // Define the primitive state
      WGPUPrimitiveState primitiveState = {};
      primitiveState.topology = WGPUPrimitiveTopology_TriangleList; // Rendering triangles
      primitiveState.stripIndexFormat = WGPUIndexFormat_Undefined;  // Non-indexed drawing
      primitiveState.frontFace = WGPUFrontFace_CW;                 // Counter-clockwise vertices define the front face
      primitiveState.cullMode = WGPUCullMode_None;//WGPUCullMode_Back;                  // Back-face culling
      
      // Multisample state
      WGPUMultisampleState multisampleState = {};
      multisampleState.count = 1;
      multisampleState.mask = ~0;
      multisampleState.alphaToCoverageEnabled = false;
      
      // Depth stencil state
      WGPUDepthStencilState depthStencilState = {};
      depthStencilState.format = WGPUTextureFormat_Depth32Float;      // Depth format
      depthStencilState.depthWriteEnabled = true;                    // Enable depth writing
      depthStencilState.depthCompare = WGPUCompareFunction_Less;     // Use less-than comparison for depth testing
      depthStencilState.stencilFront.compare = WGPUCompareFunction_Always;
      depthStencilState.stencilFront.failOp = WGPUStencilOperation_Keep;
      depthStencilState.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
      depthStencilState.stencilFront.passOp = WGPUStencilOperation_Keep;
      depthStencilState.stencilBack = depthStencilState.stencilFront; // Same as front
      depthStencilState.stencilReadMask = 0xFFFFFFFF;
      depthStencilState.stencilWriteMask = 0xFFFFFFFF;
      depthStencilState.depthBias = 0;                               // No depth bias
      depthStencilState.depthBiasSlopeScale = 0.0f;
      depthStencilState.depthBiasClamp = 0.0f;
      
      // Create the render pipeline descriptor
      WGPURenderPipelineDescriptor pipelineDesc = {};
      pipelineDesc.label = "Render Pipeline";
      pipelineDesc.layout = pipelineLayout;
      pipelineDesc.vertex = vertexState;
      pipelineDesc.primitive = primitiveState;
      pipelineDesc.fragment = &fragmentState;
      pipelineDesc.depthStencil = &depthStencilState; // Enable depth-stencil state
      pipelineDesc.multisample = multisampleState;
      
      // Finally, create the pipeline
      return wgpuDeviceCreateRenderPipeline(smState.gpuDevice, &pipelineDesc);
   };
   
   for (int i=0; i<ModelPipeline_Count; i++)
   {
      ret.pipelines[i] = buildPipelineForState((ModelPipelineState)i);
   }
   
   return ret;
}

int GFXSetup(SDL_Window* window, SDL_Renderer* renderer)
{
   smState.window = window;
   smState.renderer = renderer;
   
   if (smState.gpuInstance == NULL)
   {
      smState.gpuInstance = wgpuCreateInstance(NULL);
   }
   
   if (smState.gpuBackendType == WGPUBackendType_Undefined)
   {
#if defined(__APPLE__)
      smState.gpuBackendType = WGPUBackendType_Metal;
#elif defined(UNIX)
      smState.gpuBackendType = WGPUBackendType_Vulkan;
      // Otherwise, try X11
#elif defined(WIN32)
      smState.gpuBackendType = WGPUBackendType_Vulkan;
#elif defined(EMSCRIPTEN)
      smState.gpuBackendType = WGPUBackendType_WebGPU;
#else
      smState.gpuBackendType = WGPUBackendType_Undefined;
#endif
   }
   
   // Need to grab adapter
   if (smState.gpuInitState < SDLState::GOT_SURFACE)
   {
      if (!smState.initWGPUSurface())
      {
         printf("Problem init'ing WGPU");
         return -1;
      }
   }
   
   if (smState.gpuInitState < SDLState::GOT_ADAPTER)
   {
      if (smState.gpuInitState < SDLState::INIT_ADAPTER)
      {
         smState.requestWGPUAdapter();
      }
      return 1;
   }
   
   if (smState.gpuInitState < SDLState::GOT_DEVICE)
   {
      if (smState.gpuInitState < SDLState::INIT_DEVICE)
      {
         smState.requestWGPUDevice();
      }
      return 1;
   }
   
   if (smState.gpuInitState < SDLState::INIT_SWAPCHAIN)
   {
      smState.initWGPUSwapchain();
   }
   
   // Now we should have everything we need to init properly
   
   ImGui_ImplWGPU_InitInfo imInfo = {};
   imInfo.Device = smState.gpuDevice;
   imInfo.NumFramesInFlight = 1;
   imInfo.RenderTargetFormat = WGPUTextureFormat_BGRA8Unorm;
   imInfo.DepthStencilFormat = smState.depthStencilFormat;
   
   smState.loadShaderModule("lineShader", sLineShaderCode);
   smState.loadShaderModule("modelShader", sModelShaderCode);
   smState.loadShaderModule("terrainShader", sTerrainShaderCode);
   
   // Init gui
   IMGUI_CHECKVERSION();
   ImGui::CreateContext();
   ImGuiIO& io = ImGui::GetIO(); (void)io;
   
   io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
   
   ImGui_ImplSDL3_InitForOther(window);
   ImGui_ImplWGPU_Init(&imInfo);
   ImGui::StyleColorsDark();
   
   int w, h;
   SDL_GetWindowSize(smState.window, &w, &h);
   smState.viewportSize = slm::vec2(w,h);
   smState.window = window;
   smState.renderer = renderer;
   
   // Make common sampler
   WGPUSamplerDescriptor samplerDesc = {};
   samplerDesc.minFilter = WGPUFilterMode_Nearest;
   samplerDesc.magFilter = WGPUFilterMode_Nearest;
   samplerDesc.addressModeU = WGPUAddressMode_Repeat;
   samplerDesc.addressModeV = WGPUAddressMode_Repeat;
   samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
   samplerDesc.maxAnisotropy = 1;
   
   smState.modelCommonSampler = wgpuDeviceCreateSampler(smState.gpuDevice, &samplerDesc);
   
   samplerDesc.minFilter = WGPUFilterMode_Linear;
   samplerDesc.magFilter = WGPUFilterMode_Linear;
   samplerDesc.addressModeU = WGPUAddressMode_Repeat;
   samplerDesc.addressModeV = WGPUAddressMode_Repeat;
   samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
   samplerDesc.maxAnisotropy = 1;
   smState.modelCommonLinearSampler = wgpuDeviceCreateSampler(smState.gpuDevice, &samplerDesc);
   
   samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
   samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
   smState.modelCommonLinearClampSampler = wgpuDeviceCreateSampler(smState.gpuDevice, &samplerDesc);
   
   // Make common uniform buffer layout
   
   // Create the bind group layout
   WGPUBindGroupLayoutEntry bindGroupLayoutEntry0 = {};
   bindGroupLayoutEntry0.binding = 0;
   bindGroupLayoutEntry0.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
   bindGroupLayoutEntry0.buffer.type = WGPUBufferBindingType_Uniform;
   bindGroupLayoutEntry0.buffer.hasDynamicOffset = true;
   bindGroupLayoutEntry0.buffer.minBindingSize = sizeof(CommonUniformStruct);
   
   WGPUBindGroupLayoutDescriptor bindGroupLayoutDesc0;
   bindGroupLayoutDesc0.label = "CommonUniformStruct Bind Group";
   bindGroupLayoutDesc0.entryCount = 1;
   bindGroupLayoutDesc0.entries = &bindGroupLayoutEntry0;
   
   smState.commonUniformLayout = wgpuDeviceCreateBindGroupLayout(smState.gpuDevice, &bindGroupLayoutDesc0);
   
   // Make the common texture buffer layout
   WGPUBindGroupLayoutEntry bindGroupLayoutEntries1[2];
   
   // Texture binding
   bindGroupLayoutEntries1[0] = {};
   bindGroupLayoutEntries1[0].binding = 0;
   bindGroupLayoutEntries1[0].visibility = WGPUShaderStage_Fragment;
   bindGroupLayoutEntries1[0].texture.sampleType = WGPUTextureSampleType_Float;
   bindGroupLayoutEntries1[0].texture.viewDimension = WGPUTextureViewDimension_2D;
   bindGroupLayoutEntries1[0].texture.multisampled = false;
   
   // Sampler binding
   bindGroupLayoutEntries1[1] = {};
   bindGroupLayoutEntries1[1].binding = 1;
   bindGroupLayoutEntries1[1].visibility = WGPUShaderStage_Fragment;
   bindGroupLayoutEntries1[1].sampler.type = WGPUSamplerBindingType_Filtering;
   
   // Uniform buffer setup
   
   smState.commonUniformBuffer = smState.allocBuffer(sizeof(CommonUniformStruct), WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform, 256);
   
   WGPUBindGroupLayoutDescriptor bindGroupLayoutDesc1 = {};
   bindGroupLayoutDesc1.label = "Texture/Sampler Bind Group Layout";
   bindGroupLayoutDesc1.entryCount = 2;
   bindGroupLayoutDesc1.entries = bindGroupLayoutEntries1;
   
   smState.commonTextureLayout = wgpuDeviceCreateBindGroupLayout(smState.gpuDevice, &bindGroupLayoutDesc1);
   
   // Terrain group layout
   
   WGPUBindGroupLayoutEntry bindGroupLayoutEntriesTER[6];
   // Terrain materials
   bindGroupLayoutEntriesTER[0] = {};
   bindGroupLayoutEntriesTER[0].binding = 0;
   bindGroupLayoutEntriesTER[0].visibility = WGPUShaderStage_Fragment;
   bindGroupLayoutEntriesTER[0].texture.sampleType = WGPUTextureSampleType_Float;
   bindGroupLayoutEntriesTER[0].texture.viewDimension = WGPUTextureViewDimension_2DArray;
   bindGroupLayoutEntriesTER[0].texture.multisampled = false;
   
   // Terrain grid
   bindGroupLayoutEntriesTER[1] = {};
   bindGroupLayoutEntriesTER[1].binding = 1;
   bindGroupLayoutEntriesTER[1].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
   bindGroupLayoutEntriesTER[1].texture.sampleType = WGPUTextureSampleType_Uint;
   bindGroupLayoutEntriesTER[1].texture.viewDimension = WGPUTextureViewDimension_2D;
   bindGroupLayoutEntriesTER[1].texture.multisampled = false;
   
   // Terrain heightmap
   bindGroupLayoutEntriesTER[2] = {};
   bindGroupLayoutEntriesTER[2].binding = 2;
   bindGroupLayoutEntriesTER[2].visibility = WGPUShaderStage_Vertex;
   bindGroupLayoutEntriesTER[2].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
   bindGroupLayoutEntriesTER[2].texture.viewDimension = WGPUTextureViewDimension_2D;
   bindGroupLayoutEntriesTER[2].texture.multisampled = false;
   
   // Terrain lightmap
   bindGroupLayoutEntriesTER[3] = {};
   bindGroupLayoutEntriesTER[3].binding = 3;
   bindGroupLayoutEntriesTER[3].visibility = WGPUShaderStage_Fragment;
   bindGroupLayoutEntriesTER[3].texture.sampleType = WGPUTextureSampleType_Float;
   bindGroupLayoutEntriesTER[3].texture.viewDimension = WGPUTextureViewDimension_2D;
   bindGroupLayoutEntriesTER[3].texture.multisampled = false;
   
   // Sampler pixel
   bindGroupLayoutEntriesTER[4] = {};
   bindGroupLayoutEntriesTER[4].binding = 4;
   bindGroupLayoutEntriesTER[4].visibility = WGPUShaderStage_Vertex;
   bindGroupLayoutEntriesTER[4].sampler.type = WGPUSamplerBindingType_NonFiltering;
   
   // Sampler linear
   bindGroupLayoutEntriesTER[5] = {};
   bindGroupLayoutEntriesTER[5].binding = 5;
   bindGroupLayoutEntriesTER[5].visibility = WGPUShaderStage_Fragment;
   bindGroupLayoutEntriesTER[5].sampler.type = WGPUSamplerBindingType_Filtering;
   
   WGPUBindGroupLayoutDescriptor bindGroupLayoutDescTER = {};
   bindGroupLayoutDescTER.label = "Terrain Bind Group Layout";
   bindGroupLayoutDescTER.entryCount = 6;
   bindGroupLayoutDescTER.entries = bindGroupLayoutEntriesTER;
   
   smState.terrainTextureLayout = wgpuDeviceCreateBindGroupLayout(smState.gpuDevice, &bindGroupLayoutDescTER);
   
   WGPUBindGroupEntry commonEntry = {};
   commonEntry.binding = 0;
   commonEntry.buffer = smState.commonUniformBuffer.buffer;
   commonEntry.offset = 0;
   commonEntry.size = sizeof(CommonUniformStruct);
   
   WGPUBindGroupDescriptor commonDesc = {};
   commonDesc.label = "CommonUniformStruct";
   commonDesc.layout = smState.commonUniformLayout;
   commonDesc.entryCount = 1;
   commonDesc.entries = &commonEntry;
   smState.commonUniformGroup = wgpuDeviceCreateBindGroup(smState.gpuDevice, &commonDesc);
   
   smState.modelProgram = buildModelProgram();
   smState.lineProgram = buildLineProgram();
   smState.terrainProgram = buildTerrainProgram();
   
   return 0;
}

SDLState::SDLState()
{
   modelCommonSampler = NULL;
   modelCommonLinearSampler = NULL;
   modelCommonLinearClampSampler = NULL;
   commonUniformLayout = NULL;
   commonTextureLayout = NULL;
   terrainTextureLayout = NULL;
   commonUniformGroup = NULL;
   
   projectionMatrix = slm::mat4(1);
   modelMatrix = slm::mat4(1);
   viewMatrix = slm::mat4(1);
   
   lightColor = slm::vec4(0);
   lightPos = slm::vec3(0);
   viewportSize = slm::vec2(0);
   
   window = NULL;
   renderer = NULL;
   
   // Core state
   gpuAdapter = NULL;
   gpuDevice = NULL;
   gpuInstance = NULL;
   gpuQueue = NULL;
   gpuSurface = NULL;
   gpuSurfaceConfig = {};
   gpuBackendType = WGPUBackendType_Undefined;
   
   // Swapchain
   gpuSurfaceTexture = {};
   gpuSurfaceTextureView = NULL;
   depthTexture = NULL;
   depthTextureView = NULL;
   depthStencilFormat = WGPUTextureFormat_Undefined;
   
   // Frame state
   renderEncoder = NULL;
   commandEncoder = NULL;
   currentPipeline = NULL;
   
   gpuInitState = (GpuInitState)0;
}


void SDLState::requestWGPUAdapter()
{
   WGPUAdapter outAdapter = NULL;
   WGPURequestAdapterOptions opts = {
      .backendType = gpuBackendType
   };
   opts.compatibleSurface = gpuSurface;
   gpuInitState = SDLState::INIT_ADAPTER;
   
   wgpuInstanceRequestAdapter(gpuInstance, &opts, [](WGPURequestAdapterStatus status, WGPUAdapter adapter, const char* message, void* userdata){
      if (status == WGPURequestAdapterStatus_Success)
         ((SDLState*)userdata)->gpuAdapter = adapter;
      ((SDLState*)userdata)->gpuInitState = SDLState::GOT_ADAPTER;
   }, this);
   
   //wgpuInstanceProcessEvents(gGPUInstance);
}

void GFXPollEvents()
{
   // TODO
}

void GFXTeardown();

extern void GFXSetCocoaWindow(SDL_Window* window, WGPUSurfaceDescriptorFromMetalLayer* s);

bool SDLState::initWGPUSurface()
{
   // In case we have any dangling resources, cleanup
   GFXTeardown();
   
   gpuAdapter = NULL;
   gpuDevice = NULL;
   gpuSurface = NULL;
   
   WGPUSurfaceDescriptor desc = {};
   WGPUChainedStruct cs = {};
   
#if defined(__APPLE__)
   
   if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "cocoa") == 0)
   {
      cs.sType = WGPUSType_SurfaceDescriptorFromMetalLayer;
      
      WGPUSurfaceDescriptorFromMetalLayer surfaceDescriptorFromMetalLayer;
      surfaceDescriptorFromMetalLayer.chain = cs;
      GFXSetCocoaWindow(window, &surfaceDescriptorFromMetalLayer);
      
      desc.label = "TorqueSurface";
      desc.nextInChain = (const WGPUChainedStruct*)&surfaceDescriptorFromMetalLayer;
   }
   
#elif defined(UNIX)

   WGPUSurfaceDescriptorFromWaylandSurface chainDescWL = {};
   
   
   if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0)
   {
      cs.sType = WGPUSType_SurfaceDescriptorFromWaylandSurface;
      
      chainDescWL.chain = cs;
      chainDescWL.display = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);;
      chainDescWL.surface = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);;
      
      desc.label = "TorqueSurface";
      desc.nextInChain = (const WGPUChainedStruct*)&chainDescWL;
   }
   
   WGPUSurfaceDescriptorFromXlibWindow chainDescX11 = {};
   
   // See if we are using X11...
   if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0)
   {
      cs.sType = WGPUSType_SurfaceDescriptorFromXlibWindow;
      
      chainDescX11.chain = cs;
      chainDescX11.display = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);;
      chainDescX11.window = SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);;
      
      desc.label = "TorqueSurface";
      desc.nextInChain = (const WGPUChainedStruct*)&chainDescX11;
   }
   
#elif defined(WIN32)
   WGPUSurfaceDescriptorFromWindowsHWND chainDescWIN32 = {};
   chainDescWIN32.chain = &cs;
   
   {
      cs.sType = WGPUSType_SurfaceDescriptorFromWindowsHWND;
      
      chainDescWIN32.chain = cs;
      chainDescWIN32.hinstance = GetModuleHandle(NULL);
      chainDescWIN32.hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);;
      
      desc.label = "TorqueSurface";
      desc.nextInChain = (const WGPUChainedStruct*)&chainDescWIN32;
   }
   
#elif defined(EMSCRIPTEN)
   
   WGPUSurfaceDescriptorFromCanvasHTMLSelector chainDescES = {};
   
   {
      cs.sType = WGPUSType_SurfaceDescriptorFromCanvasHTMLSelector;
      
      chainDescES.chain = cs;
      chainDescES.selector = "canvas";
      
      desc.label = "TorqueSurface";
      desc.nextInChain = (const WGPUChainedStruct*)&chainDescES;
   }
#endif
   
   // Make surface
   
   gpuSurface = wgpuInstanceCreateSurface(gpuInstance, &desc);
   return gpuSurface != NULL;
}

// Function to request the WebGPU device
void SDLState::requestWGPUDevice()
{
   WGPUDevice outDevice = NULL;
   WGPUDeviceDescriptor deviceDesc = {};
   
   deviceDesc.label = "TVDevice";
   deviceDesc.requiredLimits = NULL;
   deviceDesc.defaultQueue.label = "TVQueue";
   
   gpuInitState = SDLState::INIT_DEVICE;
   
   wgpuAdapterRequestDevice(gpuAdapter, &deviceDesc, [](WGPURequestDeviceStatus status, WGPUDevice device, char const * message, WGPU_NULLABLE void * userdata){
      if (status == WGPURequestDeviceStatus_Success)
         ((SDLState*)userdata)->gpuDevice = device;
      ((SDLState*)userdata)->gpuInitState = SDLState::GOT_DEVICE;
      ((SDLState*)userdata)->gpuQueue = wgpuDeviceGetQueue(device);
   }, this);
}

bool SDLState::initWGPUSwapchain()
{
   resetWGPUSwapChain();
   
   // Configure presentation
   WGPUSurfaceCapabilities surfaceCapabilities = {};
   wgpuSurfaceGetCapabilities(gpuSurface, gpuAdapter, &surfaceCapabilities);
   
   // Determine render size
   backingSize[0] = 0;
   backingSize[1] = 0;
   backingScale = 1.0f;
   int windowSize[2];
   windowSize[0] = 0;
   windowSize[1] = 0;
   
   SDL_Renderer* render = SDL_GetRenderer(window);
   SDL_GetWindowSize(window, &windowSize[0], &windowSize[1]);
#if SDL_VERSION_ATLEAST(3, 0, 0)
   SDL_GetWindowSizeInPixels(window, &backingSize[0], &backingSize[1]);
#else
   backingSize[0] = windowSize[0];
   backingSize[1] = windowSize[1];
#endif
   
   backingScale = (float)backingSize[0] / (float)windowSize[0];
   
   printf("WebGPU SwapChain configured backingSize=(%u,%u), windowSize=(%u,%u)\n",
          backingSize[0], backingSize[1], windowSize[0], windowSize[1]);
   
   // Configure surface
   gpuSurfaceConfig = {};
   gpuSurfaceConfig.device = gpuDevice;
   gpuSurfaceConfig.usage = WGPUTextureUsage_RenderAttachment;
   gpuSurfaceConfig.format = WGPUTextureFormat_BGRA8Unorm; // should match pipeline
   gpuSurfaceConfig.viewFormatCount = 1;
   gpuSurfaceConfig.viewFormats = &gpuSurfaceConfig.format;
   gpuSurfaceConfig.presentMode = WGPUPresentMode_Fifo;
   gpuSurfaceConfig.alphaMode = WGPUCompositeAlphaMode_Opaque;
   gpuSurfaceConfig.width = backingSize[0];
   gpuSurfaceConfig.height = backingSize[1];
   
   wgpuSurfaceConfigure(gpuSurface, &gpuSurfaceConfig);
   
   // Make depth
   
   WGPUTextureDescriptor depthTextureDesc = {};
   depthTextureDesc.label = "Depth Texture";
   depthTextureDesc.size.width = backingSize[0];
   depthTextureDesc.size.height = backingSize[1];
   depthTextureDesc.size.depthOrArrayLayers = 1;
   depthTextureDesc.mipLevelCount = 1;
   depthTextureDesc.sampleCount = 1;
   depthTextureDesc.dimension = WGPUTextureDimension_2D;
   depthTextureDesc.format = WGPUTextureFormat_Depth32Float;
   depthTextureDesc.usage = WGPUTextureUsage_RenderAttachment;
   
   depthTexture = wgpuDeviceCreateTexture(gpuDevice, &depthTextureDesc);
   depthStencilFormat = WGPUTextureFormat_Depth32Float;
   
   // Create a texture view from the depth texture
   depthTextureView = wgpuTextureCreateView(depthTexture, NULL);
   
   return true;
}

void GFXResetSwapChain()
{
   if (smState.depthTextureView == NULL)
      return;
   
   smState.resetWGPUSwapChain();
   smState.initWGPUSwapchain();
}

void SDLState::resetWGPUSwapChain()
{
   if (gpuSurfaceTextureView)
   {
      wgpuTextureViewRelease(gpuSurfaceTextureView);
      gpuSurfaceTexture = {};
      gpuSurfaceTextureView = NULL;
   }
   
   if (depthTextureView)
   {
      wgpuTextureRelease(depthTexture);
      wgpuTextureViewRelease(depthTextureView);
      depthTexture = NULL;
      depthTextureView = NULL;
   }
}

void SDLState::resetWGPUState()
{
   resetWGPUSwapChain();
   
   lineProgram.reset();
   modelProgram.reset();
   
   if (commonUniformGroup)
   {
      wgpuBindGroupRelease(commonUniformGroup);
      wgpuBindGroupLayoutRelease(commonUniformLayout);
      wgpuBindGroupLayoutRelease(commonTextureLayout);
      wgpuBindGroupLayoutRelease(terrainTextureLayout);
   }
   
   for (auto& itr : shaders)
   {
      wgpuShaderModuleRelease(itr.second);
   }
   
   for (auto& itr : buffers)
   {
      wgpuBufferRelease(itr.buffer);
   }
   
   if (gpuDevice)
      wgpuDeviceRelease(gpuDevice);
   if (gpuAdapter)
      wgpuAdapterRelease(gpuAdapter);
   if (gpuSurface)
      wgpuSurfaceRelease(gpuSurface);
   
   gpuDevice = NULL;
   gpuAdapter = NULL;
   gpuSurface = NULL;
   
   shaders.clear();
   buffers.clear();
   
   modelCommonSampler = NULL;
   commonUniformLayout = NULL;
   commonTextureLayout = NULL;
   terrainTextureLayout = NULL;
   commonUniformGroup = NULL;
}

WGPUBindGroup SDLState::makeSimpleTextureBG(WGPUTextureView tex, WGPUSampler sampler)
{
   WGPUBindGroupEntry bindGroupEntries[2];
   
   // Texture entry
   bindGroupEntries[0] = {};
   bindGroupEntries[0].binding = 0;
   bindGroupEntries[0].textureView = tex;
   
   // Sampler entry
   bindGroupEntries[1] = {};
   bindGroupEntries[1].binding = 1;
   bindGroupEntries[1].sampler = sampler;
   
   WGPUBindGroupDescriptor bindGroupDesc = {};
   bindGroupDesc.label = "SimpleBindGoup";
   bindGroupDesc.layout = smState.commonTextureLayout;
   bindGroupDesc.entryCount = 2;
   bindGroupDesc.entries = bindGroupEntries;
   
   // Create the bind group
   return wgpuDeviceCreateBindGroup(gpuDevice, &bindGroupDesc);
}

WGPUBindGroup SDLState::makeTerrainTextureBG(WGPUTextureView squareMatTex, WGPUTextureView heightmapTex, WGPUTextureView mapTex, WGPUTextureView lmTex, WGPUSampler samplerPixel, WGPUSampler samplerLinear)
{
   WGPUBindGroupEntry bindGroupEntries[6];
   
   // Texture entry
   bindGroupEntries[0] = {};
   bindGroupEntries[0].binding = 0;
   bindGroupEntries[0].textureView = squareMatTex;
   // Texture entry
   bindGroupEntries[1] = {};
   bindGroupEntries[1].binding = 1;
   bindGroupEntries[1].textureView = mapTex;
   // Texture entry
   bindGroupEntries[2] = {};
   bindGroupEntries[2].binding = 2;
   bindGroupEntries[2].textureView = heightmapTex;
   // Texture entry
   bindGroupEntries[3] = {};
   bindGroupEntries[3].binding = 3;
   bindGroupEntries[3].textureView = lmTex;
   
   // Sampler entry
   bindGroupEntries[4] = {};
   bindGroupEntries[4].binding = 4;
   bindGroupEntries[4].sampler = samplerPixel;
   // Sampler entry
   bindGroupEntries[5] = {};
   bindGroupEntries[5].binding = 5;
   bindGroupEntries[5].sampler = samplerLinear;
   
   WGPUBindGroupDescriptor bindGroupDesc = {};
   bindGroupDesc.label = "TerrainLayout";
   bindGroupDesc.layout = smState.terrainTextureLayout;
   bindGroupDesc.entryCount = 6;
   bindGroupDesc.entries = bindGroupEntries;
   
   // Create the bind group
   return wgpuDeviceCreateBindGroup(gpuDevice, &bindGroupDesc);
}

// Create the render pass
WGPURenderPassDescriptor SDLState::createRenderPass(bool secondary)
{
   // Color attachment
   static WGPURenderPassColorAttachment colorAttachment = {};
   colorAttachment.view = gpuSurfaceTextureView;
   colorAttachment.resolveTarget = NULL;  // No MSAA
   colorAttachment.loadOp = secondary ? WGPULoadOp_Load : WGPULoadOp_Clear;  // Clear the color buffer at the start
   colorAttachment.storeOp = WGPUStoreOp_Store; // Store the color output
   colorAttachment.clearValue = (WGPUColor){0.0, 0.0, 0.0, 1.0}; // Clear to black with full opacity
   
   // Depth attachment
   static WGPURenderPassDepthStencilAttachment depthAttachment = {};
   depthAttachment.view = depthTextureView;
   depthAttachment.depthLoadOp = WGPULoadOp_Clear;  // Clear the depth buffer
   depthAttachment.depthStoreOp = WGPUStoreOp_Store; // Store depth after rendering
   depthAttachment.depthClearValue = 1.0f;
   depthAttachment.stencilLoadOp = WGPULoadOp_Clear; // Optional if you are using stencil
   depthAttachment.stencilStoreOp = WGPUStoreOp_Discard;
   depthAttachment.stencilClearValue = 0;
   
   WGPURenderPassDescriptor renderPassDesc = {};
   renderPassDesc.colorAttachmentCount = 1;
   renderPassDesc.colorAttachments = &colorAttachment;
   renderPassDesc.depthStencilAttachment = &depthAttachment; // Attach the depth texture
   
   return renderPassDesc;
}

bool SDLState::loadShaderModule(const char* name, const char* code)
{
   WGPUShaderModuleWGSLDescriptor wgslDesc = {};
   wgslDesc.chain.sType = WGPUSType_ShaderModuleWGSLDescriptor;
   wgslDesc.code = code;
   
   WGPUShaderModuleDescriptor shaderModuleDesc = {};
   shaderModuleDesc.nextInChain = (const WGPUChainedStruct*)&wgslDesc;
   
   WGPUShaderModule shaderModule = wgpuDeviceCreateShaderModule(gpuDevice, &shaderModuleDesc);
   if (!shaderModule)
   {
      return false;
   }
   else
   {
      shaders[name] = shaderModule;
      return true;
   }
}

static const size_t BufferSize = 1024*1024*10;

SDLState::BufferRef SDLState::allocBuffer(size_t size, uint32_t flags, uint16_t alignment)
{
   
   for (SDLState::BufferAlloc& alloc : buffers)
   {
      if (alloc.flags != flags)
         continue;
      
      alloc.head = AlignSize(alloc.head, alignment);
      size_t nextSize = alloc.head + size;
      nextSize = AlignSize(nextSize, alignment);
      if (nextSize > alloc.size)
         continue;
      
      SDLState::BufferRef ref;
      ref.buffer = alloc.buffer;
      ref.offset = alloc.head;
      ref.size = size;
      
      alloc.head = nextSize;
      return ref;
   }
   
   WGPUBufferDescriptor bufferDesc = {};
   bufferDesc.size = BufferSize;
   bufferDesc.usage = flags;
   bufferDesc.mappedAtCreation = false;
   
   BufferAlloc newAlloc = {};
   newAlloc.flags = flags;
   newAlloc.size = bufferDesc.size;
   newAlloc.buffer = wgpuDeviceCreateBuffer(smState.gpuDevice, &bufferDesc);
   buffers.push_back(newAlloc);
   
   return allocBuffer(size, flags, alignment);
}

void SDLState::resetBufferAllocs()
{
   for (SDLState::BufferAlloc& alloc : buffers)
   {
      alloc.head = 0;
   }
}

void SDLState::beginRenderPass(bool secondary)
{
   if (renderEncoder != NULL)
      return;

   WGPUCommandEncoderDescriptor desc = {};
   desc.label = "FrameEncoder";
   commandEncoder = wgpuDeviceCreateCommandEncoder(gpuDevice, &desc);

   WGPURenderPassDescriptor renderPassDesc = createRenderPass(secondary);
   renderEncoder = wgpuCommandEncoderBeginRenderPass(commandEncoder, &renderPassDesc);
}

void SDLState::endRenderPass()
{
   if (renderEncoder == NULL)
      return;
   
   // End the render pass
   wgpuRenderPassEncoderEnd(renderEncoder);
   
   // Finish the command encoder to submit the work
   WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(commandEncoder, NULL);
   
   // NOTE: these both need to be released here otherwise wgpuQueueSubmit complains
   
   // Finished with this
   wgpuRenderPassEncoderRelease(renderEncoder);
   renderEncoder = NULL;
   
   // Finished with this
   wgpuCommandEncoderRelease(commandEncoder);
   commandEncoder = NULL;
   
   // Submit the command buffer to the GPU queue
   wgpuQueueSubmit(gpuQueue, 1, &commandBuffer);
   
   //wgpuQueueOnSubmittedWorkDone(WGPUQueue queue, WGPUQueueOnSubmittedWorkDoneCallback callback, WGPU_NULLABLE void * userdata) WGPU_FUNCTION_ATTRIBUTE;
   
   wgpuCommandBufferRelease(commandBuffer);
   
   currentPipeline = NULL;
}

void GFXTeardown()
{
   if (smState.gpuDevice == NULL)
      return;
   
   ImGui_ImplWGPU_Shutdown();
   ImGui_ImplSDL3_Shutdown();
   smState.resetWGPUState();
}

void GFXTestRender(slm::vec3 pos)
{
   
}

bool GFXBeginFrame()
{
   if (smState.commandEncoder)
      return false;
   
   for (SDLState::FrameModel& model : smState.models)
   {
      model.inFrame = false;
   }
   
   // Re-use last texture if still present
   if (smState.gpuSurfaceTexture.texture == NULL)
   {
      // Grab tex
      wgpuSurfaceGetCurrentTexture(smState.gpuSurface, &smState.gpuSurfaceTexture);
      
      // Check status
      switch (smState.gpuSurfaceTexture.status)
      {
         case WGPUSurfaceGetCurrentTextureStatus_Success:
            break;
         default:
         {
            return false;
         }
      }
      
      WGPUTextureViewDescriptor viewDescriptor = {};
      viewDescriptor.label = "WindowSurfaceView";
      viewDescriptor.format = wgpuTextureGetFormat(smState.gpuSurfaceTexture.texture);
      viewDescriptor.dimension = WGPUTextureViewDimension_2D;
      viewDescriptor.baseMipLevel = 0;
      viewDescriptor.mipLevelCount = 1;
      viewDescriptor.arrayLayerCount = 1;
      viewDescriptor.aspect = WGPUTextureAspect_All;
      
      // NOTE: if we decide later not to prepare the frame, it should end up getting released
      // in changeDevice.
      wgpuTextureReference(smState.gpuSurfaceTexture.texture);
      smState.gpuSurfaceTextureView = wgpuTextureCreateView(smState.gpuSurfaceTexture.texture, &viewDescriptor);
   }

   smState.beginRenderPass(false);
   
   ImGui_ImplWGPU_NewFrame();
   ImGui_ImplSDL3_NewFrame();
   ImGui::NewFrame();
   
   return true;
}

void GFXEndFrame()
{
   smState.endRenderPass();
   
   // Render imgui
   
   smState.beginRenderPass(true);
   ImGui::EndFrame();
   ImGui::Render();
   ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), smState.renderEncoder);
   
   smState.endRenderPass();
   
   if (smState.gpuSurfaceTexture.texture)
   {
      wgpuTextureRelease(smState.gpuSurfaceTexture.texture);
      wgpuTextureViewRelease(smState.gpuSurfaceTextureView);
      smState.gpuSurfaceTextureView = NULL;
      smState.gpuSurfaceTexture = {};
   }
   
   wgpuSurfacePresent(smState.gpuSurface);
   
   smState.resetBufferAllocs();
}

void GFXHandleResize()
{
   int w, h;
   SDL_GetWindowSize(smState.window, &w, &h);
   slm::vec2 newSize = slm::vec2(w,h);
   
   if (newSize != smState.viewportSize)
   {
      smState.viewportSize = newSize;
      
      // Reset swap chain
      GFXResetSwapChain();
   }
}

int32_t GFXLoadCustomTexture(CustomTextureFormat fmt, uint32_t width, uint32_t height, void* data)
{
   uint8_t* texData = NULL;
   uint32_t pow2W = getNextPow2(width);
   uint32_t pow2H = getNextPow2(height);
   WGPUTextureFormat pixFormat = WGPUTextureFormat_Undefined;
   
   uint32_t bpp = 4;
   bool is565 = false;
   
   switch (fmt)
   {
      case CustomTexture_Float:
         bpp = 4;
         pixFormat = WGPUTextureFormat_R32Float;
         break;
      case CustomTexture_RG8:
         bpp = 2;
         pixFormat = WGPUTextureFormat_RG8Uint;
         break;
      case CustomTexture_RGBA8:
         bpp = 4;
         pixFormat = WGPUTextureFormat_RGBA8Unorm;
         break;
      case CustomTexture_LM16: // 565
         bpp = 4;
         pixFormat = WGPUTextureFormat_RGBA8Unorm;
         is565 = true;
         break;
      case CustomTexture_TerrainSquare:
         bpp = 2;
         pixFormat = WGPUTextureFormat_R16Uint;
         break;
      default:
         assert(false);
         return -1;
   }
   
   uint32_t paddedWidth = (uint32_t)AlignSize(pow2W*bpp, 256);
   uint32_t alignedMipSize = paddedWidth * pow2H;
   
   texData = new uint8_t[alignedMipSize];
   memset(texData, 0, alignedMipSize);
   
   if (!is565)
   {
      copyMipDirect(height, width*bpp, paddedWidth, (uint8_t*)data, texData);
   }
   else
   {
      copyLMMipDirect(height, width*2, paddedWidth, (uint8_t*)data, texData);
   }
   
   WGPUTexture tex;
   if (texData)
   {
      // Create the texture
      WGPUTextureDescriptor textureDesc = {};
      //textureDesc.label = "Texture";
      textureDesc.size = (WGPUExtent3D){pow2W, pow2H, 1};
      textureDesc.mipLevelCount = 1;      // Corresponds to GL_TEXTURE_BASE_LEVEL = 0 and GL_TEXTURE_MAX_LEVEL = 0
      textureDesc.sampleCount = 1;
      textureDesc.dimension = WGPUTextureDimension_2D;
      textureDesc.format = pixFormat;  // Corresponds to GL_RGBA in OpenGL
      textureDesc.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
      tex = wgpuDeviceCreateTexture(smState.gpuDevice, &textureDesc);
      
      // Create the texture view
      WGPUTextureViewDescriptor textureViewDesc = {};
      //textureViewDesc.label = "Texture View";
      textureViewDesc.format = pixFormat;
      textureViewDesc.dimension = WGPUTextureViewDimension_2D;
      textureViewDesc.mipLevelCount = 1;
      textureViewDesc.arrayLayerCount = 1;
      WGPUTextureView texView = wgpuTextureCreateView(tex, &textureViewDesc);
      
      // Upload texture data
      WGPUTextureDataLayout layout = {};
      layout.offset = 0;
      layout.bytesPerRow = paddedWidth;
      layout.rowsPerImage = pow2H;
      WGPUExtent3D size = {pow2W, pow2H, 1};
      
      WGPUImageCopyTexture copyInfo = {};
      copyInfo.texture = tex;
      copyInfo.mipLevel = 0;
      copyInfo.origin = (WGPUOrigin3D){0, 0, 0};
      copyInfo.aspect = WGPUTextureAspect_All;
      
      wgpuQueueWriteTexture(smState.gpuQueue,
                            &copyInfo,
                            texData,
                            alignedMipSize, // Assuming padded 4 bytes per pixel (RGBA8 format)
                            &layout,
                            &size);
      
      // Clean up texture data after uploading
      delete[] texData;
      
      SDLState::TexInfo newInfo = {};
      newInfo.texture = tex;
      newInfo.textureView = texView;
      newInfo.texBindGroup = NULL;
      newInfo.dims[0] = textureDesc.size.width;
      newInfo.dims[1] = textureDesc.size.height;
      newInfo.dims[2] = textureDesc.size.depthOrArrayLayers;
      
      // Find or add texture to smState.textures
      int sz = smState.textures.size();
      for (int i = 0; i < sz; i++)
      {
         if (smState.textures[i].texture == NULL)
         {
            smState.textures[i] = newInfo;
            return i;
         }
      }
      
      smState.textures.push_back(newInfo);
      return (uint32_t)(smState.textures.size() - 1);
   }
   else
   {
      // Handle the case where texture creation failed
      delete[] texData;
      return -1;
   }
   
   return -1;
}

int32_t GFXLoadTexture(Bitmap* bmp, Palette* defaultPal)
{
   uint8_t* texData = NULL;
   uint32_t pow2W = getNextPow2(bmp->mWidth);
   uint32_t pow2H = getNextPow2(bmp->mHeight);
   WGPUTextureFormat pixFormat = WGPUTextureFormat_Undefined;
   
   uint32_t paddedWidth = (uint32_t)AlignSize(pow2W*4, 256);
   uint32_t alignedMipSize = paddedWidth * pow2H;
   
   if (bmp->mBitDepth == 8)
   {
      Palette::Data* pal = NULL;
      
      if (bmp->mPal)
         pal = bmp->mPal->getPaletteByIndex(bmp->mPaletteIndex);
      else if (defaultPal && defaultPal->mPalettes.size() > 0)
         pal = defaultPal->getPaletteByIndex(bmp->mPaletteIndex);
      else
      {
         printf("No default palette specified\n");
         assert(false);
         return false;
      }
      
      if (bmp->mFlags & Bitmap::FLAG_TRANSPARENT)
      {
         texData = new uint8_t[alignedMipSize];
         copyMipRGBA(bmp->mWidth, bmp->mHeight, paddedWidth, pal, bmp->mMips[0], texData, 255);
      }
      else if (bmp->mFlags & Bitmap::FLAG_TRANSLUCENT)
      {
         texData = new uint8_t[alignedMipSize];
         copyMipRGBA(bmp->mWidth, bmp->mHeight, paddedWidth, pal, bmp->mMips[0], texData, 1);
      }
      else
      {
         texData = new uint8_t[alignedMipSize];
         copyMipRGBA(bmp->mWidth, bmp->mHeight, paddedWidth, pal, bmp->mMips[0], texData, 256);
      }
   }
   else if (bmp->mBitDepth == 24)
   {
      uint8_t* texData = new uint8_t[alignedMipSize];
      copyMipDirectPadded(bmp->mHeight, bmp->getStride(bmp->mWidth), paddedWidth, bmp->mMips[0], texData);
   }
   else
   {
      assert(false);
      return -1;
   }
   
   WGPUTexture tex;
   if (texData)
   {
      // Create the texture
      WGPUTextureDescriptor textureDesc = {};
      //textureDesc.label = "Texture";
      textureDesc.size = (WGPUExtent3D){pow2W, pow2H, 1};
      textureDesc.mipLevelCount = 1;      // Corresponds to GL_TEXTURE_BASE_LEVEL = 0 and GL_TEXTURE_MAX_LEVEL = 0
      textureDesc.sampleCount = 1;
      textureDesc.dimension = WGPUTextureDimension_2D;
      textureDesc.format = bmp->mBGR ? WGPUTextureFormat_BGRA8Unorm : WGPUTextureFormat_RGBA8Unorm;  // Corresponds to GL_RGBA in OpenGL
      textureDesc.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
      tex = wgpuDeviceCreateTexture(smState.gpuDevice, &textureDesc);
      
      // Create the texture view
      WGPUTextureViewDescriptor textureViewDesc = {};
      //textureViewDesc.label = "Texture View";
      textureViewDesc.format = bmp->mBGR ? WGPUTextureFormat_BGRA8Unorm : WGPUTextureFormat_RGBA8Unorm;  // Same as the texture format
      textureViewDesc.dimension = WGPUTextureViewDimension_2D;
      textureViewDesc.mipLevelCount = 1;
      textureViewDesc.arrayLayerCount = 1;
      WGPUTextureView texView = wgpuTextureCreateView(tex, &textureViewDesc);
      
      // Upload texture data
      WGPUTextureDataLayout layout = {};
      layout.offset = 0;
      layout.bytesPerRow = paddedWidth;
      layout.rowsPerImage = pow2H;
      WGPUExtent3D size = {pow2W, pow2H, 1};
      
      WGPUImageCopyTexture copyInfo = {};
      copyInfo.texture = tex;
      copyInfo.mipLevel = 0;
      copyInfo.origin = (WGPUOrigin3D){0, 0, 0};
      copyInfo.aspect = WGPUTextureAspect_All;
      
      wgpuQueueWriteTexture(smState.gpuQueue,
                            &copyInfo,
                            texData,
                            alignedMipSize, // Assuming padded 4 bytes per pixel (RGBA8 format)
                            &layout,
                            &size);
      
      // Clean up texture data after uploading
      delete[] texData;
      
      SDLState::TexInfo newInfo = {};
      newInfo.texture = tex;
      newInfo.textureView = texView;
      newInfo.dims[0] = textureDesc.size.width;
      newInfo.dims[1] = textureDesc.size.height;
      newInfo.dims[2] = textureDesc.size.depthOrArrayLayers;
      newInfo.texBindGroup = smState.makeSimpleTextureBG(texView, smState.modelCommonSampler);
      
      // Find or add texture to smState.textures
      int sz = smState.textures.size();
      for (int i = 0; i < sz; i++)
      {
         if (smState.textures[i].texture == NULL)
         {
            smState.textures[i] = newInfo;
            return i;
         }
      }
      
      smState.textures.push_back(newInfo);
      return (uint32_t)(smState.textures.size() - 1);
   }
   else
   {
      // Handle the case where texture creation failed
      delete[] texData;
      return -1;
   }
   
   return -1;
}

int32_t GFXLoadTextureSet(uint32_t numBitmaps, Bitmap** bmps, Palette* defaultPal)
{
    if (numBitmaps == 0 || bmps == nullptr)
        return -1;

    Bitmap* firstBmp = bmps[0];
    uint32_t pow2W = getNextPow2(firstBmp->mWidth);
    uint32_t pow2H = getNextPow2(firstBmp->mHeight);
    uint32_t paddedWidth = (uint32_t)AlignSize(pow2W * 4, 256);
    uint32_t alignedMipSize = paddedWidth * pow2H;
    
    // Allocate memory to store texture data for each bitmap layer
    uint8_t** texDataArray = new uint8_t*[numBitmaps];

    for (uint32_t i = 0; i < numBitmaps; ++i)
    {
        Bitmap* bmp = bmps[i];
        Palette::Data* pal = nullptr;
        
        if (bmp->mBitDepth == 8)
        {
            if (bmp->mPal)
                pal = bmp->mPal->getPaletteByIndex(bmp->mPaletteIndex);
            else if (defaultPal && defaultPal->mPalettes.size() > 0)
                pal = defaultPal->getPaletteByIndex(bmp->mPaletteIndex);
            else
            {
                printf("No default palette specified\n");
                assert(false);
                return -1;
            }

            // Allocate texture data for the current bitmap
            texDataArray[i] = new uint8_t[alignedMipSize];

            // Depending on flags, copy the texture data
            if (bmp->mFlags & Bitmap::FLAG_TRANSPARENT)
            {
                copyMipRGBA(bmp->mWidth, bmp->mHeight, paddedWidth, pal, bmp->mMips[0], texDataArray[i], 255);
            }
            else if (bmp->mFlags & Bitmap::FLAG_TRANSLUCENT)
            {
                copyMipRGBA(bmp->mWidth, bmp->mHeight, paddedWidth, pal, bmp->mMips[0], texDataArray[i], 1);
            }
            else
            {
                copyMipRGBA(bmp->mWidth, bmp->mHeight, paddedWidth, pal, bmp->mMips[0], texDataArray[i], 256);
            }
        }
        else if (bmp->mBitDepth == 24)
        {
            texDataArray[i] = new uint8_t[alignedMipSize];
            copyMipDirectPadded(bmp->mHeight, bmp->getStride(bmp->mWidth), paddedWidth, bmp->mMips[0], texDataArray[i]);
        }
        else
        {
            assert(false);
            return -1;
        }
    }

    // Create the 2D texture array with numBitmaps layers
    WGPUTexture tex;
    WGPUTextureDescriptor textureDesc = {};
    textureDesc.size = (WGPUExtent3D){pow2W, pow2H, numBitmaps};  // Use numBitmaps for the depth (layer count)
    textureDesc.mipLevelCount = 1;
    textureDesc.sampleCount = 1;
    textureDesc.dimension = WGPUTextureDimension_2D;
    textureDesc.format = firstBmp->mBGR ? WGPUTextureFormat_BGRA8Unorm : WGPUTextureFormat_RGBA8Unorm;
    textureDesc.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
    tex = wgpuDeviceCreateTexture(smState.gpuDevice, &textureDesc);

    // Upload texture data for each layer
    for (uint32_t i = 0; i < numBitmaps; ++i)
    {
        WGPUTextureDataLayout layout = {};
        layout.offset = 0;
        layout.bytesPerRow = paddedWidth;
        layout.rowsPerImage = pow2H;

        WGPUExtent3D size = {pow2W, pow2H, 1};

        WGPUImageCopyTexture copyInfo = {};
        copyInfo.texture = tex;
        copyInfo.mipLevel = 0;
        copyInfo.origin = (WGPUOrigin3D){0, 0, 0};
        copyInfo.aspect = WGPUTextureAspect_All;
        copyInfo.origin.z = i;  // Target the ith layer of the texture

        wgpuQueueWriteTexture(smState.gpuQueue,
                              &copyInfo,
                              texDataArray[i],
                              alignedMipSize,  // Padded 4 bytes per pixel (RGBA8 format)
                              &layout,
                              &size);
    }

    // Clean up the texture data after uploading
    for (uint32_t i = 0; i < numBitmaps; ++i)
    {
        delete[] texDataArray[i];
    }
    delete[] texDataArray;

    // Create a texture view for each layer (if necessary)
    WGPUTextureViewDescriptor textureViewDesc = {};
    textureViewDesc.format = firstBmp->mBGR ? WGPUTextureFormat_BGRA8Unorm : WGPUTextureFormat_RGBA8Unorm;
    textureViewDesc.dimension = WGPUTextureViewDimension_2DArray;
    textureViewDesc.mipLevelCount = 1;
    textureViewDesc.arrayLayerCount = numBitmaps;

    WGPUTextureView texView = wgpuTextureCreateView(tex, &textureViewDesc);

    // Store the texture and return the index
    SDLState::TexInfo newInfo = {};
    newInfo.texture = tex;
    newInfo.textureView = texView;
    newInfo.dims[0] = textureDesc.size.width;
    newInfo.dims[1] = textureDesc.size.height;
    newInfo.dims[2] = textureDesc.size.depthOrArrayLayers;
    newInfo.texBindGroup = NULL;//smState.makeSimpleTextureBG(texView, smState.modelCommonSampler);

    // Find or add texture to smState.textures
    int sz = smState.textures.size();
    for (int i = 0; i < sz; i++)
    {
        if (smState.textures[i].texture == NULL)
        {
            smState.textures[i] = newInfo;
            return i;
        }
    }

    smState.textures.push_back(newInfo);
    return (uint32_t)(smState.textures.size() - 1);
}


void GFXDeleteTexture(int32_t texID)
{
   if (texID < 0 || texID >= smState.textures.size())
      return;
   
   SDLState::TexInfo& tex = smState.textures[texID];
   if (tex.texture == NULL)
      return;
   
   wgpuTextureViewRelease(tex.textureView);
   wgpuTextureRelease(tex.texture);
   
   tex.texture = NULL;
   tex.textureView = NULL;
}

void GFXLoadModelData(uint32_t modelId, void* verts, void* texverts, void* inds, uint32_t numVerts, uint32_t numTexVerts, uint32_t numInds)
{
   SDLState::FrameModel blankModel = {};
   while (smState.models.size() <= modelId)
      smState.models.push_back(blankModel);
   
   SDLState::FrameModel& model = smState.models[modelId];
   model.inFrame = false;
   
   if (model.vertData)
      delete[] model.vertData;
   if (model.texVertData)
      delete[] model.texVertData;
   if (model.indexData)
      delete[] model.indexData;
   
   model.vertData = new ModelVertex[numVerts];
   model.texVertData = new ModelTexVertex[numTexVerts];
   model.indexData = new uint16_t[numInds];
   
   model.numVerts = numVerts;
   model.numTexVerts = numTexVerts;
   model.numInds = numInds;
   
   memcpy(model.vertData, verts, sizeof(ModelVertex) * numVerts);
   memcpy(model.texVertData, texverts, sizeof(ModelTexVertex) * numTexVerts);
   memcpy(model.indexData, inds, sizeof(uint16_t) * numInds);
}

void GFXClearModelData(uint32_t modelId)
{
   if (smState.models.size() <= modelId)
      return;
   
   SDLState::FrameModel& model = smState.models[modelId];
   model.inFrame = false;
   
   if (model.vertData)
      delete[] model.vertData;
   if (model.texVertData)
      delete[] model.texVertData;
   if (model.indexData)
      delete[] model.indexData;
   
   model.vertData = NULL;
   model.texVertData = NULL;
   model.indexData = NULL;
   
   model.numVerts = 0;
   model.numTexVerts = 0;
   model.numInds = 0;
}

void GFXSetModelViewProjection(slm::mat4 &model, slm::mat4 &view, slm::mat4 &proj)
{
   smState.modelMatrix = model;
   smState.projectionMatrix = proj;
   smState.viewMatrix = view;
   
   CommonUniformStruct& uniforms = smState.currentProgram->uniforms;
   uniforms.projMat = smState.projectionMatrix;
   
   if (smState.currentPipeline == smState.lineProgram.pipeline)
   {
      uniforms.modelMat = slm::mat4(1);
      uniforms.viewMat = smState.viewMatrix;
   }
   else
   {
      uniforms.modelMat = smState.modelMatrix;
      uniforms.viewMat = smState.viewMatrix;
   }
}

void GFXSetLightPos(slm::vec3 pos, slm::vec4 ambient)
{
   smState.lightPos = pos;
   smState.lightColor = ambient;
   
   if (smState.currentPipeline != smState.lineProgram.pipeline)
   {
      smState.currentProgram->uniforms.lightPos = slm::vec4(pos.x, pos.y, pos.z, 0.0f);
      smState.currentProgram->uniforms.lightColor = slm::vec4(ambient.x, ambient.y, ambient.z, ambient.w);
   }
}

void GFXBeginModelPipelineState(ModelPipelineState state, int32_t texID, float testVal)
{
   smState.currentPipeline = smState.modelProgram.pipelines[state];
   smState.currentProgram = &smState.modelProgram;
   wgpuRenderPassEncoderSetPipeline(smState.renderEncoder, smState.currentPipeline);
   
   GFXSetLightPos(smState.lightPos, smState.lightColor);
   GFXSetModelViewProjection(smState.modelMatrix, smState.viewMatrix, smState.projectionMatrix);
   
   if (state == ModelPipeline_DefaultDiffuse)
   {
      smState.modelProgram.uniforms.params2.x = testVal;
   }
   else
   {
      smState.modelProgram.uniforms.params2.x = 1.1f;
   }
   
   // Set texture
   SDLState::TexInfo& info = smState.textures[texID];
   wgpuRenderPassEncoderSetBindGroup(smState.renderEncoder, 1, info.texBindGroup, 0, NULL);
}

void GFXSetModelVerts(uint32_t modelId, uint32_t vertOffset, uint32_t texOffset)
{
   SDLState::FrameModel& model = smState.models[modelId];
   const size_t vertSize = sizeof(ModelVertex) * model.numVerts;
   const size_t texVertSize = sizeof(ModelTexVertex) * model.numTexVerts;
   const size_t indexSize = AlignSize(sizeof(uint16_t) * model.numInds, sizeof(uint32_t));
   
   if (model.inFrame == false)
   {
      model.indexOffset = smState.allocBuffer(model.numInds * sizeof(uint16_t), WGPUBufferUsage_CopyDst | WGPUBufferUsage_Index, sizeof(uint32_t));
      wgpuQueueWriteBuffer(smState.gpuQueue, model.indexOffset.buffer, model.indexOffset.offset, model.indexData, indexSize);
      
      model.vertOffset = smState.allocBuffer(vertSize, WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex, sizeof(ModelVertex));
      wgpuQueueWriteBuffer(smState.gpuQueue, model.vertOffset.buffer, model.vertOffset.offset, model.vertData, vertSize);
      
      model.texVertOffset = smState.allocBuffer(texVertSize, WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex, sizeof(ModelTexVertex));
      wgpuQueueWriteBuffer(smState.gpuQueue, model.texVertOffset.buffer, model.texVertOffset.offset, model.texVertData, texVertSize);
      
      // Load in frame
      model.inFrame = true;
   }
   
   wgpuRenderPassEncoderSetIndexBuffer(smState.renderEncoder, model.indexOffset.buffer, WGPUIndexFormat_Uint16, model.indexOffset.offset, model.numInds * sizeof(uint16_t));
       
   wgpuRenderPassEncoderSetVertexBuffer(smState.renderEncoder, 0, model.vertOffset.buffer, model.vertOffset.offset + vertOffset, vertSize);
   wgpuRenderPassEncoderSetVertexBuffer(smState.renderEncoder, 1, model.texVertOffset.buffer, model.texVertOffset.offset + texOffset, texVertSize);
}

void GFXDrawModelVerts(uint32_t numVerts, uint32_t startVerts)
{
   SDLState::BufferRef uniformData = smState.allocBuffer(sizeof(CommonUniformStruct), WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform, 256);
   wgpuQueueWriteBuffer(smState.gpuQueue, uniformData.buffer, uniformData.offset, &smState.currentProgram->uniforms, sizeof(CommonUniformStruct));
   
   uint32_t offsets[1];
   offsets[0] = uniformData.offset;
   wgpuRenderPassEncoderSetBindGroup(smState.renderEncoder, 0, smState.commonUniformGroup, 1, offsets);
   
   wgpuRenderPassEncoderDraw(smState.renderEncoder, numVerts, 1, startVerts, 0);
}

void GFXDrawModelPrims(uint32_t numVerts, uint32_t numInds, uint32_t startInds, uint32_t startVerts)
{
   SDLState::BufferRef uniformData = smState.allocBuffer(sizeof(CommonUniformStruct), WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform, 256);
   wgpuQueueWriteBuffer(smState.gpuQueue, uniformData.buffer, uniformData.offset, &smState.currentProgram->uniforms, sizeof(CommonUniformStruct));
   
   uint32_t offsets[1];
   offsets[0] = uniformData.offset;
   wgpuRenderPassEncoderSetBindGroup(smState.renderEncoder, 0, smState.commonUniformGroup, 1, offsets);
   
   wgpuRenderPassEncoderDrawIndexed(smState.renderEncoder, numInds, 1, startInds, startVerts, 0);
}

void GFXSetTerrainResources(uint32_t terrainID, int32_t matTexListID, int32_t heightMapTexID, int32_t gridMapTexID, int32_t lightmapTexID)
{
   SDLState::TerrainGPUResource blankRes = {};
   while (smState.terrainResources.size() <= terrainID)
      smState.terrainResources.push_back(blankRes);
   
   SDLState::TerrainGPUResource& res = smState.terrainResources[terrainID];
   if (res.mBindGroup == NULL || 
       (res.mMatListTexID != matTexListID) || 
        (res.mGridMapTexID != gridMapTexID) ||
        (res.mLightMapTexID != lightmapTexID))
   {
      if (res.mBindGroup != NULL)
      {
         wgpuBindGroupRelease(res.mBindGroup);
      }
      
      WGPUTextureView squareMatView = smState.textures[matTexListID].textureView;
      WGPUTextureView heightMapView = smState.textures[heightMapTexID].textureView;
      WGPUTextureView gridMapView = smState.textures[gridMapTexID].textureView;
      WGPUTextureView lightMapView = smState.textures[lightmapTexID].textureView;
      smState.terrainProgram.uniforms.params2.w = smState.textures[lightmapTexID].dims[0];
      
      res.mBindGroup = smState.makeTerrainTextureBG(squareMatView, heightMapView, gridMapView, lightMapView, smState.modelCommonSampler, smState.modelCommonLinearClampSampler);
   }
}

void GFXBeginTerrainPipelineState(TerrainPipelineState state, uint32_t terrainID, float squareSize, float gridX, float gridY, const slm::vec4* matCoords)
{
   smState.currentPipeline = smState.terrainProgram.pipelines[state];
   smState.currentProgram = &smState.terrainProgram;
   wgpuRenderPassEncoderSetPipeline(smState.renderEncoder, smState.currentPipeline);
   
   smState.terrainProgram.uniforms.params2.y = squareSize;
   smState.terrainProgram.uniforms.params2.z = gridX;
   
   SDLState::TerrainGPUResource& res = smState.terrainResources[terrainID];
   
   wgpuRenderPassEncoderSetBindGroup(smState.renderEncoder, 1, res.mBindGroup, 0, NULL);
   
   memcpy(smState.currentProgram->uniforms.squareTexCoords, matCoords, sizeof(slm::vec4)*16);
   
   GFXSetModelViewProjection(smState.modelMatrix, smState.viewMatrix, smState.projectionMatrix);
}

void GFXBeginLinePipelineState()
{
   smState.currentPipeline = smState.lineProgram.pipeline;
   smState.currentProgram = &smState.lineProgram;
   wgpuRenderPassEncoderSetPipeline(smState.renderEncoder, smState.currentPipeline);
   
   GFXSetModelViewProjection(smState.modelMatrix, smState.viewMatrix, smState.projectionMatrix);
}

void GFXDrawLine(slm::vec3 start, slm::vec3 end, slm::vec4 color, float width)
{
   _LineVert verts[6];
   verts[0].pos = start;
   verts[0].nextPos = end;
   verts[0].normal = slm::vec3(-1,0,0); // b
   verts[0].color = color;
   verts[1].pos = start;
   verts[1].nextPos = end;
   verts[1].normal = slm::vec3(1,0,0); // t
   verts[1].color = color;
   verts[2].pos = end;
   verts[2].nextPos = start;
   verts[2].normal = slm::vec3(1,0,0); // t
   verts[2].color = color;
   
   verts[3].pos = end;
   verts[3].nextPos = start;
   verts[3].normal = slm::vec3(1,0,0); // t
   verts[3].color = color;
   verts[4].pos = end;
   verts[4].nextPos = start;
   verts[4].normal = slm::vec3(-1,0,0); // b
   verts[4].color = color;
   verts[5].pos = start;
   verts[5].nextPos = end;
   verts[5].normal = slm::vec3(1,0,0); // b
   verts[5].color = color;
   
   smState.lineProgram.uniforms.params1 = slm::vec4(1.0f / smState.viewportSize.x, 1.0f / smState.viewportSize.y, width, 0.0f);
   
   SDLState::BufferRef uniformData = smState.allocBuffer(sizeof(CommonUniformStruct), WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform, 256);
   wgpuQueueWriteBuffer(smState.gpuQueue, uniformData.buffer, uniformData.offset, &smState.lineProgram.uniforms, sizeof(CommonUniformStruct));
   
   SDLState::BufferRef lineData = smState.allocBuffer(sizeof(verts), WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex, sizeof(_LineVert));
   wgpuQueueWriteBuffer(smState.gpuQueue, lineData.buffer, lineData.offset, verts, sizeof(verts));
   
   uint32_t offsets[1];
   offsets[0] = uniformData.offset;
   wgpuRenderPassEncoderSetBindGroup(smState.renderEncoder, 0, smState.commonUniformGroup, 1, offsets);
   
   wgpuRenderPassEncoderSetVertexBuffer(smState.renderEncoder, 0, lineData.buffer, lineData.offset, sizeof(verts));
   
   wgpuRenderPassEncoderDraw(smState.renderEncoder, 6, 1, 0, 0);
}
