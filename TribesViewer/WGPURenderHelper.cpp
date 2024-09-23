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


extern "C"
{
#if defined(__APPLE__) || defined(WGPU_NATIVE)
#include "webgpu.h"
#else
#include <webgpu/webgpu.h>
#endif
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
};

struct ProgramInfo
{
   WGPURenderPipeline pipelines[ModelPipeline_Count];
   CommonUniformStruct uniforms;
   
   ProgramInfo() { memset(this, '\0', sizeof(ProgramInfo)); }
   
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

struct LineProgramInfo
{
   WGPURenderPipeline pipeline;
   CommonUniformStruct uniforms;
   
   LineProgramInfo() { memset(this, '\0', sizeof(LineProgramInfo)); }
   
   void reset()
   {
      if (pipeline)
         wgpuRenderPipelineRelease(pipeline);
      pipeline = NULL;
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
   };
   
   std::vector<FrameModel> models;
   std::vector<TexInfo> textures;
   
   // Resource state
   std::unordered_map<std::string, WGPUShaderModule> shaders;
   std::vector<BufferAlloc> buffers;
   
   WGPUSampler modelCommonSampler;
   WGPUBindGroupLayout commonUniformLayout;
   WGPUBindGroupLayout commonTextureLayout;
   WGPUBindGroup commonUniformGroup;
   
   LineProgramInfo lineProgram;
   ProgramInfo modelProgram;
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
   WGPURenderPipeline currentPipeline;
   
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
   
   void beginRenderPass();
   void endRenderPass();
   
   WGPUBindGroup makeSimpleTextureBG(WGPUTextureView tex, WGPUSampler sampler);
   WGPURenderPassDescriptor createRenderPass();
};


SDLState smState;


static LineProgramInfo buildLineProgram()
{
   LineProgramInfo ret;
   
   // Create the pipeline layout
   WGPUPipelineLayoutDescriptor pipelineLayoutDesc;
   pipelineLayoutDesc.label = "Pipeline Layout";
   pipelineLayoutDesc.bindGroupLayoutCount = 1;
   WGPUBindGroupLayout bindGroupLayouts[1] = {smState.commonUniformLayout};
   pipelineLayoutDesc.bindGroupLayouts = bindGroupLayouts;
   
   WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(smState.gpuDevice, &pipelineLayoutDesc);
   
   // Vertex buffer layout for stream 0 (LineVertex)
   WGPUVertexAttribute vertexAttributes[4];
   
   // Position attribute
   vertexAttributes[0].format = WGPUVertexFormat_Float32x3;
   vertexAttributes[0].offset = offsetof(LineVertex, position);
   vertexAttributes[0].shaderLocation = 0; // Corresponding to `position` in the shader
   
   // Next position attribute
   vertexAttributes[1].format = WGPUVertexFormat_Float32x3;
   vertexAttributes[1].offset = offsetof(LineVertex, nextPosition);
   vertexAttributes[1].shaderLocation = 1; // Corresponding to `nextPosition` in the shader
   
   // Normal attribute
   vertexAttributes[2].format = WGPUVertexFormat_Float32x3;
   vertexAttributes[2].offset = offsetof(LineVertex, normal);
   vertexAttributes[2].shaderLocation = 2; // Corresponding to `normal` in the shader
   
   // Color attribute (packed_float4)
   vertexAttributes[3].format = WGPUVertexFormat_Float32x4;
   vertexAttributes[3].offset = offsetof(LineVertex, color);
   vertexAttributes[3].shaderLocation = 3; // Corresponding to `color` in the shader
   
   // Define the vertex buffer layout
   WGPUVertexBufferLayout vertexBufferLayout;
   vertexBufferLayout.arrayStride = sizeof(LineVertex);
   vertexBufferLayout.stepMode = WGPUVertexStepMode_Vertex; // Per-vertex data
   vertexBufferLayout.attributeCount = 4;
   vertexBufferLayout.attributes = vertexAttributes;
   
   // Vertex state configuration
   WGPUVertexState vertexState;
   vertexState.module = smState.shaders["lineShader"];  // Loaded shader module
   vertexState.entryPoint = "vertMain";          // Entry point of the vertex shader
   vertexState.bufferCount = 1;
   vertexState.buffers = &vertexBufferLayout;
   
   // Create the bind group layout for group 0 (if needed)
   WGPUBindGroupLayoutEntry bindGroupLayoutEntry0;
   bindGroupLayoutEntry0.binding = 0;
   bindGroupLayoutEntry0.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
   bindGroupLayoutEntry0.buffer.type = WGPUBufferBindingType_Uniform;
   bindGroupLayoutEntry0.buffer.minBindingSize = sizeof(CommonUniformStruct);
   
   WGPUBindGroupLayoutDescriptor bindGroupLayoutDesc0;
   bindGroupLayoutDesc0.label = "Uniform Bind Group Layout";
   bindGroupLayoutDesc0.entryCount = 1;
   bindGroupLayoutDesc0.entries = &bindGroupLayoutEntry0;
   
   WGPUBindGroupLayout bindGroupLayout0 = wgpuDeviceCreateBindGroupLayout(smState.gpuDevice, &bindGroupLayoutDesc0);
   
   // Fragment state
   WGPUFragmentState fragmentState;
   fragmentState.module = smState.shaders["lineShader"]; // Loaded fragment shader module
   fragmentState.entryPoint = "fragMain";           // Entry point of the fragment shader
   fragmentState.targetCount = 1;
   
   WGPUColorTargetState colorTargetState;
   colorTargetState.format = WGPUTextureFormat_RGBA8Unorm; // Output texture format
   colorTargetState.blend = NULL;                          // No blending
   colorTargetState.writeMask = WGPUColorWriteMask_All;    // Write all color channels
   
   fragmentState.targets = &colorTargetState;
   
   // Define the primitive state (we assume you are drawing lines, so topology is LineList)
   WGPUPrimitiveState primitiveState;
   primitiveState.topology = WGPUPrimitiveTopology_TriangleList;
   primitiveState.stripIndexFormat = WGPUIndexFormat_Undefined;  // Non-indexed drawing
   primitiveState.frontFace = WGPUFrontFace_CW;                  // Not relevant since there's no culling
   primitiveState.cullMode = WGPUCullMode_None;                  // No culling for lines
   
   // Multisample state
   WGPUMultisampleState multisampleState;
   multisampleState.count = 1;
   multisampleState.mask = ~0;
   multisampleState.alphaToCoverageEnabled = false;
   
   // Since there is no depth testing, we omit the depthStencil state
   
   // Create the render pipeline descriptor
   WGPURenderPipelineDescriptor pipelineDesc;
   pipelineDesc.label = "Render Pipeline";
   pipelineDesc.layout = pipelineLayout;
   pipelineDesc.vertex = vertexState;
   pipelineDesc.primitive = primitiveState;
   pipelineDesc.fragment = &fragmentState;
   pipelineDesc.depthStencil = NULL; // No depth stencil state
   pipelineDesc.multisample = multisampleState;
   
   // Finally, create the pipeline
   ret.pipeline = wgpuDeviceCreateRenderPipeline(smState.gpuDevice, &pipelineDesc);
   
   return ret;
}

ProgramInfo buildProgram()
{
   ProgramInfo ret;
   
   // Create the pipeline layout
   WGPUPipelineLayoutDescriptor pipelineLayoutDesc;
   pipelineLayoutDesc.label = "Pipeline Layout";
   pipelineLayoutDesc.bindGroupLayoutCount = 2;
   WGPUBindGroupLayout bindGroupLayouts[2] = {smState.commonUniformLayout, smState.commonTextureLayout};
   pipelineLayoutDesc.bindGroupLayouts = bindGroupLayouts;
   
   WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(smState.gpuDevice, &pipelineLayoutDesc);
   
   auto buildPipelineForState = [&pipelineLayout](ModelPipelineState state){
      // Vertex buffer layout for stream 0 (ModelVertex)
      WGPUVertexAttribute vertexAttributes0[2];
      vertexAttributes0[0].format = WGPUVertexFormat_Float32x3;
      vertexAttributes0[0].offset = offsetof(ModelVertex, position);
      vertexAttributes0[0].shaderLocation = 0;
      
      vertexAttributes0[1].format = WGPUVertexFormat_Float32x3;
      vertexAttributes0[1].offset = offsetof(ModelVertex, normal);
      vertexAttributes0[1].shaderLocation = 1;
      
      WGPUVertexBufferLayout vertexBufferLayout0;
      vertexBufferLayout0.arrayStride = sizeof(ModelVertex);
      vertexBufferLayout0.stepMode = WGPUVertexStepMode_Vertex; // Per-vertex data
      vertexBufferLayout0.attributeCount = 2;
      vertexBufferLayout0.attributes = vertexAttributes0;
      
      // Vertex buffer layout for stream 1 (ModelTexVertex)
      WGPUVertexAttribute vertexAttributes1[1];
      vertexAttributes1[0].format = WGPUVertexFormat_Float32x2;
      vertexAttributes1[0].offset = offsetof(ModelTexVertex, texcoord);
      vertexAttributes1[0].shaderLocation = 2;
      
      WGPUVertexBufferLayout vertexBufferLayout1;
      vertexBufferLayout1.arrayStride = sizeof(ModelTexVertex);
      vertexBufferLayout1.stepMode = WGPUVertexStepMode_Vertex; // Per-vertex data
      vertexBufferLayout1.attributeCount = 1;
      vertexBufferLayout1.attributes = vertexAttributes1;
      
      // Vertex state configuration
      WGPUVertexState vertexState;
      vertexState.module = smState.shaders["modelShader"];  // Loaded shader module
      vertexState.entryPoint = "vertMain";          // Entry point of the vertex shader
      vertexState.bufferCount = 2;
      WGPUVertexBufferLayout vertexLayouts[2] = {vertexBufferLayout0, vertexBufferLayout1};
      vertexState.buffers = vertexLayouts;
      
      // Create the pipeline layout
      WGPUPipelineLayoutDescriptor pipelineLayoutDesc;
      pipelineLayoutDesc.label = "Pipeline Layout";
      pipelineLayoutDesc.bindGroupLayoutCount = 2;
      WGPUBindGroupLayout bindGroupLayouts[2] = {smState.commonUniformLayout, smState.commonTextureLayout};
      pipelineLayoutDesc.bindGroupLayouts = bindGroupLayouts;
      
      WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(smState.gpuDevice, &pipelineLayoutDesc);
      
      // Fragment state
      WGPUFragmentState fragmentState;
      fragmentState.module = smState.shaders["modelShader"]; // Loaded fragment shader module
      fragmentState.entryPoint = "fragMain";         // Entry point of the fragment shader
      fragmentState.targetCount = 1;
      
      WGPUColorTargetState colorTargetState;
      colorTargetState.format = WGPUTextureFormat_BGRA8Unorm; // Output texture format
      
      WGPUBlendState blendState;
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
      
      colorTargetState.blend = &blendState;
      colorTargetState.writeMask = WGPUColorWriteMask_All;
      
      fragmentState.targets = &colorTargetState;
      
      // Define the primitive state
      WGPUPrimitiveState primitiveState;
      primitiveState.topology = WGPUPrimitiveTopology_TriangleList; // Rendering triangles
      primitiveState.stripIndexFormat = WGPUIndexFormat_Undefined;  // Non-indexed drawing
      primitiveState.frontFace = WGPUFrontFace_CW;                 // Counter-clockwise vertices define the front face
      primitiveState.cullMode = WGPUCullMode_Back;                  // Back-face culling
      
      // Multisample state
      WGPUMultisampleState multisampleState;
      multisampleState.count = 1;
      multisampleState.mask = ~0;
      multisampleState.alphaToCoverageEnabled = false;
      
      // Depth stencil state
      WGPUDepthStencilState depthStencilState;
      depthStencilState.format = WGPUTextureFormat_Depth24Plus;      // Depth format
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
      WGPURenderPipelineDescriptor pipelineDesc;
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
#if defined(SDL_VIDEO_DRIVER_COCOA) || defined(SDL_VIDEO_DRIVER_UIKIT)
      smState.gpuBackendType = WGPUBackendType_Metal;
#elif defined(SDL_VIDEO_DRIVER_X11) || defined(SDL_VIDEO_DRIVER_WAYLAND)
      smState.gpuBackendType = WGPUBackendType_Vulkan;
      // Otherwise, try X11
#elif defined(SDL_VIDEO_DRIVER_WINDOWS)
      smState.gpuBackendType = WGPUBackendType_Vulkan;
#elif defined(SDL_VIDEO_DRIVER_EMSCRIPTEN)
      smState.gpuBackendType = WGPUBackendType_WebGPU;
#else
      smState.gpuBackendType = WGPUBackendType_Undefined;
#endif
   }
   
   // Need to grab adapter
   if (smState.gpuInitState < SDLState::GOT_SURFACE)
   {
      smState.initWGPUSurface();
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
   
   // Init gui
   IMGUI_CHECKVERSION();
   ImGui::CreateContext();
   ImGuiIO& io = ImGui::GetIO(); (void)io;
   
   ImGui_ImplSDL3_InitForOther(window);
   ImGui_ImplWGPU_Init(&imInfo);
   ImGui::StyleColorsDark();
   
   int w, h;
   SDL_GetWindowSize(smState.window, &w, &h);
   smState.viewportSize = slm::vec2(w,h);
   smState.window = window;
   smState.renderer = renderer;
   
   smState.modelProgram = buildProgram();
   smState.lineProgram = buildLineProgram();
   
   // Make common sampler
   WGPUSamplerDescriptor samplerDesc = {};
   samplerDesc.minFilter = WGPUFilterMode_Nearest;
   samplerDesc.magFilter = WGPUFilterMode_Nearest;
   samplerDesc.addressModeU = WGPUAddressMode_Repeat;
   samplerDesc.addressModeV = WGPUAddressMode_Repeat;
   samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
   
   smState.modelCommonSampler = wgpuDeviceCreateSampler(smState.gpuDevice, &samplerDesc);
   
   // Make common uniform buffer layout
   
   // Create the bind group layout
   WGPUBindGroupLayoutEntry bindGroupLayoutEntry0;
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
   bindGroupLayoutEntries1[0].binding = 0;
   bindGroupLayoutEntries1[0].visibility = WGPUShaderStage_Fragment;
   bindGroupLayoutEntries1[0].texture.sampleType = WGPUTextureSampleType_Float;
   bindGroupLayoutEntries1[0].texture.viewDimension = WGPUTextureViewDimension_2D;
   bindGroupLayoutEntries1[0].texture.multisampled = false;
   
   // Sampler binding
   bindGroupLayoutEntries1[1].binding = 1;
   bindGroupLayoutEntries1[1].visibility = WGPUShaderStage_Fragment;
   bindGroupLayoutEntries1[1].sampler.type = WGPUSamplerBindingType_Filtering;
   
   WGPUBindGroupLayoutDescriptor bindGroupLayoutDesc1;
   bindGroupLayoutDesc1.label = "Texture/Sampler Bind Group Layout";
   bindGroupLayoutDesc1.entryCount = 2;
   bindGroupLayoutDesc1.entries = bindGroupLayoutEntries1;
   
   smState.commonTextureLayout = wgpuDeviceCreateBindGroupLayout(smState.gpuDevice, &bindGroupLayoutDesc1);
   
   /*
    
    smState.currentProgram = smState.modelProgram.programID;
    
    uint32_t VertexArrayID;
    glGenVertexArrays(1, &VertexArrayID);
    glBindVertexArray(VertexArrayID);
    */
   
   return 0;
}

SDLState::SDLState()
{
   modelCommonSampler = NULL;
   commonUniformLayout = NULL;
   commonTextureLayout = NULL;
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

bool SDLState::initWGPUSurface()
{
   // In case we have any dangling resources, cleanup
   GFXTeardown();
   
   gpuAdapter = NULL;
   gpuDevice = NULL;
   gpuSurface = NULL;
   
   WGPUSurfaceDescriptor desc = {};
   WGPUChainedStruct cs = {};
   
#if defined(SDL_VIDEO_DRIVER_COCOA)
   // TODO
#elif defined(SDL_VIDEO_DRIVER_UIKIT)
   // TODO
#elif defined(SDL_VIDEO_DRIVER_X11) || defined(SDL_VIDEO_DRIVER_WAYLAND)
   
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
   WGPUSurfaceDescriptorFromWaylandSurface chainDescWL = {};
   
   
   if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0)
   {
      struct wl_display *display = (struct wl_display *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
      struct wl_surface *surface = (struct wl_surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
      
      cs.sType = WGPUSType_SurfaceDescriptorFromWaylandSurface;
      
      chainDescWL.chain = cs;
      chainDescWL.display = wayland_display;
      chainDescWL.surface = wayland_surface;
      
      desc.label = "TorqueSurface";
      desc.nextInChain = (const WGPUChainedStruct*)&chainDescWL;
   }
#endif
   
#if defined(SDL_VIDEO_DRIVER_X11)
   WGPUSurfaceDescriptorFromXlibWindow chainDescX11 = {};
   
   // See if we are using X11...
   if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0)
   {
      Display *xdisplay = (Display *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
      Window xwindow = (Window)SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
      
      
      cs.sType = WGPUSType_SurfaceDescriptorFromXlibWindow;
      
      chainDescX11.chain = cs;
      chainDescX11.display = x11_display;
      chainDescX11.window = x11_window;
      
      desc.label = "TorqueSurface";
      desc.nextInChain = (const WGPUChainedStruct*)&chainDescX11;
   }
#endif
   
#elif defined(SDL_VIDEO_DRIVER_WINDOWS)
   WGPUSurfaceDescriptorFromWindowsHWND chainDescWIN32 = {};
   chainDescWIN32.chain = &cs;
   
   {
      HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
      HINSTANCE hInstance = GetModuleHandle(NULL);
      
      cs.sType = WGPUSType_SurfaceDescriptorFromWindowsHWND;
      
      chainDescWIN32.chain = cs;
      chainDescWIN32.hinstance = hInstance;
      chainDescWIN32.hwnd = hwnd;
      
      desc.label = "TorqueSurface";
      desc.nextInChain = (const WGPUChainedStruct*)&chainDescWIN32;
   }
   
#elif defined(SDL_VIDEO_DRIVER_EMSCRIPTEN)
   
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
   
   printf("WebGPU SwapChain configured backingSize=(%u,%u), windowSize=(%u,%u)",
          backingSize[0], backingSize[1], windowSize[0], windowSize[1]);
   
   // Configure surface
   gpuSurfaceConfig = {};
   gpuSurfaceConfig.device = gpuDevice;
   gpuSurfaceConfig.usage = WGPUTextureUsage_RenderAttachment;
   gpuSurfaceConfig.format = WGPUTextureFormat_BGRA8Unorm; // should match pipeline
   //mWGPUSurfaceConfig.viewFormatCount = 1;
   //mWGPUSurfaceConfig.viewFormats = &mWGPUSurfaceConfig.format;
   gpuSurfaceConfig.presentMode = WGPUPresentMode_Fifo;
   gpuSurfaceConfig.alphaMode = WGPUCompositeAlphaMode_Opaque;
   gpuSurfaceConfig.width = backingSize[0];
   gpuSurfaceConfig.height = backingSize[1];
   
   wgpuSurfaceConfigure(gpuSurface, &gpuSurfaceConfig);
   
   // Make depth
   
   WGPUTextureDescriptor depthTextureDesc;
   depthTextureDesc.label = "Depth Texture";
   depthTextureDesc.size.width = backingSize[0];
   depthTextureDesc.size.height = backingSize[1];
   depthTextureDesc.size.depthOrArrayLayers = 1;
   depthTextureDesc.mipLevelCount = 1;
   depthTextureDesc.sampleCount = 1;
   depthTextureDesc.dimension = WGPUTextureDimension_2D;
   depthTextureDesc.format = WGPUTextureFormat_Depth24Plus;
   depthTextureDesc.usage = WGPUTextureUsage_RenderAttachment;
   
   depthTexture = wgpuDeviceCreateTexture(gpuDevice, &depthTextureDesc);
   
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
   commonUniformGroup = NULL;
}

WGPUBindGroup SDLState::makeSimpleTextureBG(WGPUTextureView tex, WGPUSampler sampler)
{
   WGPUBindGroupEntry bindGroupEntries[2];
   
   // Texture entry (binding 0)
   bindGroupEntries[0].binding = 0;
   bindGroupEntries[0].textureView = tex;
   bindGroupEntries[0].sampler = NULL;  // Not a sampler binding, leave this NULL
   bindGroupEntries[0].buffer = NULL;   // Not a buffer binding, leave this NULL
   
   // Sampler entry (binding 1)
   bindGroupEntries[1].binding = 1;
   bindGroupEntries[1].textureView = NULL;  // Not a texture view binding, leave this NULL
   bindGroupEntries[1].sampler = sampler;
   bindGroupEntries[1].buffer = NULL;       // Not a buffer binding, leave this NULL
   
   WGPUBindGroupDescriptor bindGroupDesc;
   bindGroupDesc.label = "Texture and Sampler Bind Group";
   bindGroupDesc.layout = smState.commonTextureLayout;
   bindGroupDesc.entryCount = 2;
   bindGroupDesc.entries = bindGroupEntries;
   
   // Create the bind group
   return wgpuDeviceCreateBindGroup(gpuDevice, &bindGroupDesc);
}

// Create the render pass
WGPURenderPassDescriptor SDLState::createRenderPass()
{
   // Color attachment
   WGPURenderPassColorAttachment colorAttachment;
   colorAttachment.view = gpuSurfaceTextureView;
   colorAttachment.resolveTarget = NULL;  // No MSAA
   colorAttachment.loadOp = WGPULoadOp_Clear;  // Clear the color buffer at the start
   colorAttachment.storeOp = WGPUStoreOp_Store; // Store the color output
   colorAttachment.clearValue = (WGPUColor){0.0, 0.0, 0.0, 1.0}; // Clear to black with full opacity
   
   // Depth attachment
   WGPURenderPassDepthStencilAttachment depthAttachment;
   depthAttachment.view = depthTextureView;
   depthAttachment.depthLoadOp = WGPULoadOp_Clear;  // Clear the depth buffer
   depthAttachment.depthStoreOp = WGPUStoreOp_Store; // Store depth after rendering
   //depthAttachment.clearDepth = 1.0f; // Clear depth to the farthest value
   depthAttachment.stencilLoadOp = WGPULoadOp_Clear; // Optional if you are using stencil
   depthAttachment.stencilStoreOp = WGPUStoreOp_Discard;
   //depthAttachment.clearStencil = 0;
   
   WGPURenderPassDescriptor renderPassDesc;
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

static const size_t BufferSize = 1024*1024*1024;

static inline size_t AlignSize(const size_t size, const uint16_t alignment)
{
   return (size + (alignment - 1)) & ~(alignment - 1);
}

SDLState::BufferRef SDLState::allocBuffer(size_t size, uint32_t flags, uint16_t alignment)
{
   
   for (SDLState::BufferAlloc& alloc : buffers)
   {
      if (alloc.flags != flags)
         continue;
      
      size_t nextSize = alloc.head + size;
      nextSize = AlignSize(nextSize, alignment);
      if (nextSize > alloc.size)
         continue;
      
      SDLState::BufferRef ref;
      ref.buffer = alloc.buffer;
      ref.offset = alloc.head;
      ref.size = size;
      return ref;
   }
   
   WGPUBufferDescriptor bufferDesc = {};
   bufferDesc.size = BufferSize;
   bufferDesc.usage = flags;
   bufferDesc.mappedAtCreation = false;
   
   BufferAlloc newAlloc = {};
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

void SDLState::beginRenderPass()
{
   if (renderEncoder != NULL)
      return;

   WGPUCommandEncoderDescriptor desc = {};
   desc.label = "FrameEncoder";
   commandEncoder = wgpuDeviceCreateCommandEncoder(gpuDevice, &desc);

   WGPURenderPassDescriptor renderPassDesc = createRenderPass();
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
   
   // Submit the command buffer to the GPU queue
   wgpuQueueSubmit(gpuQueue, 1, &commandBuffer);
   
   wgpuCommandBufferRelease(commandBuffer);   
   wgpuRenderPassEncoderRelease(renderEncoder);
   wgpuCommandEncoderRelease(commandEncoder);
   
   renderEncoder = NULL;
   commandEncoder = NULL;
   currentPipeline = NULL;
}

void GFXTeardown()
{
   ImGui_ImplWGPU_Shutdown();
   ImGui_ImplSDL3_Shutdown();
   smState.resetWGPUState();
}

void GFXTestRender(slm::vec3 pos)
{
   
}

bool GFXBeginFrame()
{
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

   smState.beginRenderPass();
   
   ImGui_ImplWGPU_NewFrame();
   ImGui_ImplSDL3_NewFrame();
   ImGui::NewFrame();
   
   return true;
}

void GFXEndFrame()
{
   ImGui::EndFrame();
   ImGui::Render();
   ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), smState.renderEncoder);
   
   smState.endRenderPass();
   
   SDL_RenderPresent(smState.renderer);
}

void GFXHandleResize()
{
   int w, h;
   SDL_GetWindowSize(smState.window, &w, &h);
   smState.viewportSize = slm::vec2(w,h);
   
   // Recreate backing textures
   
}


int32_t GFXLoadTexture(Bitmap* bmp, Palette* defaultPal)
{
   uint8_t* texData = NULL;
   uint32_t pow2W = getNextPow2(bmp->mWidth);
   uint32_t pow2H = getNextPow2(bmp->mHeight);
   WGPUTextureFormat pixFormat = WGPUTextureFormat_Undefined;
   
   uint32_t alignedMipSize = AlignSize(pow2W*pow2H*4, 256);
   
   if (bmp->mBitDepth == 8)
   {
      Palette::Data* pal = NULL;
      
      if (bmp->mPal)
         pal = bmp->mPal->getPaletteByIndex(bmp->mPaletteIndex);
      else if (defaultPal && defaultPal->mPalettes.size() > 0)
         pal = defaultPal->getPaletteByIndex(bmp->mPaletteIndex);
      else
      {
         assert(false);
         return false;
      }
      
      if (bmp->mFlags & Bitmap::FLAG_TRANSPARENT)
      {
         texData = new uint8_t[alignedMipSize];
         copyMipRGBA(bmp->mWidth, bmp->mHeight, pow2W*4, pal, bmp->mMips[0], texData, 255);
      }
      else if (bmp->mFlags & Bitmap::FLAG_TRANSLUCENT)
      {
         texData = new uint8_t[alignedMipSize];
         copyMipRGBA(bmp->mWidth, bmp->mHeight, pow2W*4, pal, bmp->mMips[0], texData, 1);
      }
      else
      {
         texData = new uint8_t[alignedMipSize];
         copyMipRGBA(bmp->mWidth, bmp->mHeight, pow2W*4, pal, bmp->mMips[0], texData, 256);
      }
   }
   else if (bmp->mBitDepth == 24)
   {
      uint8_t* texData = new uint8_t[alignedMipSize];
      copyMipDirectPadded(bmp->mHeight, bmp->getStride(bmp->mWidth), pow2W*3, bmp->mMips[0], texData);
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
      textureViewDesc.format = WGPUTextureFormat_RGBA8Unorm;  // Same as the texture format
      textureViewDesc.dimension = WGPUTextureViewDimension_2D;
      textureViewDesc.mipLevelCount = 1;
      textureViewDesc.arrayLayerCount = 1;
      WGPUTextureView texView = wgpuTextureCreateView(tex, &textureViewDesc);
      
      // Upload texture data
      WGPUTextureDataLayout layout = {};
      layout.offset = 0;
      layout.bytesPerRow = AlignSize(pow2W * 4, 256);
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
                            pow2W * pow2H * 4, // Assuming 4 bytes per pixel (RGBA8 format)
                            &layout,
                            &size);
      
      // Clean up texture data after uploading
      delete[] texData;
      
      SDLState::TexInfo newInfo = {};
      newInfo.texture = tex;
      newInfo.textureView = texView;
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
   memcpy(model.texVertData, verts, sizeof(ModelTexVertex) * numTexVerts);
   memcpy(model.indexData, verts, sizeof(uint16_t) * numInds);
}

void GFXSetModelViewProjection(slm::mat4 &model, slm::mat4 &view, slm::mat4 &proj)
{
   smState.modelMatrix = model;
   smState.projectionMatrix = proj;
   smState.viewMatrix = view;
   
   CommonUniformStruct& uniforms = (smState.currentPipeline == smState.lineProgram.pipeline) ?
   smState.lineProgram.uniforms : smState.modelProgram.uniforms;
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
      smState.modelProgram.uniforms.lightPos = slm::vec4(pos.x, pos.y, pos.z, 0.0f);
      smState.modelProgram.uniforms.lightColor = slm::vec4(ambient.x, ambient.y, ambient.z, ambient.w);
   }
}

void GFXBeginModelPipelineState(ModelPipelineState state, int32_t texID, float testVal)
{
   smState.currentPipeline = smState.modelProgram.pipelines[state];
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
   
   if (model.inFrame == false)
   {
      model.vertOffset = smState.allocBuffer(vertSize, WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex, sizeof(ModelVertex));
      wgpuQueueWriteBuffer(smState.gpuQueue, model.vertOffset.buffer, model.vertOffset.offset, model.vertData, vertSize);
      
      model.texVertOffset = smState.allocBuffer(texVertSize, WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex, sizeof(ModelTexVertex));
      wgpuQueueWriteBuffer(smState.gpuQueue, model.texVertOffset.buffer, model.texVertOffset.offset, model.vertData, texVertSize);
      
      // Load in frame
      model.inFrame = true;
   }
   
   wgpuRenderPassEncoderSetVertexBuffer(smState.renderEncoder, 0, model.vertOffset.buffer, model.vertOffset.offset, vertSize);
   wgpuRenderPassEncoderSetVertexBuffer(smState.renderEncoder, 1, model.texVertOffset.buffer, model.texVertOffset.offset, texVertSize);
}

void GFXDrawModelPrims(uint32_t numVerts, uint32_t numInds, uint32_t startInds, uint32_t startVerts)
{
   SDLState::BufferRef uniformData = smState.allocBuffer(sizeof(CommonUniformStruct), WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform, 256);
   wgpuQueueWriteBuffer(smState.gpuQueue, uniformData.buffer, uniformData.offset, &smState.modelProgram.uniforms, sizeof(CommonUniformStruct));
   
   uint32_t offsets[1];
   offsets[0] = uniformData.offset;
   wgpuRenderPassEncoderSetBindGroup(smState.renderEncoder, 0, smState.commonUniformGroup, 1, offsets);
   
   wgpuRenderPassEncoderDrawIndexed(smState.renderEncoder, numInds, 1, startInds, startVerts, 0);
}

void GFXBeginLinePipelineState()
{
   smState.currentPipeline = smState.lineProgram.pipeline;
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
   
   smState.lineProgram.uniforms.params1 = slm::vec4(smState.viewportSize.x, smState.viewportSize.y, width, 0.0f);
   
   SDLState::BufferRef uniformData = smState.allocBuffer(sizeof(CommonUniformStruct), WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform, 256);
   wgpuQueueWriteBuffer(smState.gpuQueue, uniformData.buffer, uniformData.offset, &smState.lineProgram.uniforms, sizeof(CommonUniformStruct));
   
   SDLState::BufferRef lineData = smState.allocBuffer(sizeof(_LineVert), WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex, sizeof(_LineVert));
   wgpuQueueWriteBuffer(smState.gpuQueue, lineData.buffer, lineData.offset, verts, sizeof(verts));
   
   uint32_t offsets[1];
   offsets[0] = uniformData.offset;
   wgpuRenderPassEncoderSetBindGroup(smState.renderEncoder, 0, smState.commonUniformGroup, 1, offsets);
   
   wgpuRenderPassEncoderSetVertexBuffer(smState.renderEncoder, 0, lineData.buffer, lineData.offset, sizeof(verts));
   
   wgpuRenderPassEncoderDraw(smState.renderEncoder, 6, 1, 0, 0);
}
