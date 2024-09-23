//
//  MetalRendererHelper.m
//  TribesViewer
//
//  Created by James Urquhart on 27/07/2018.
//  Copyright © 2018 James Urquhart. All rights reserved.
//

#ifdef USE_METAL
#include <simd/simd.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>
#include <unordered_map>
#include <slm/slmath.h>
#import "CommonShaderTypes.h"
#import "CommonData.h"
#import "RendererHelper.h"
#import "CommonShaderTypes.h"
#import "QuartzCore/CAMetalLayer.h"


#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_metal.h"


#import <Cocoa/Cocoa.h>
#include "SDL.h"

#include <simd/simd.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

class SDL_Renderer;

@class CAMetalLayer;
@interface MetalRendererHelper : NSObject

@property(nonatomic, retain) id<MTLDevice> device;
@property(nonatomic, retain) id<MTLCommandQueue> commandQueue;
@property(nonatomic, retain) id<MTLRenderPipelineState> pipelineState;

@property(nonatomic, retain) CAMetalLayer* layer;

@property(nonatomic, assign) vector_uint2 viewportSize;

@property(nonatomic, strong) dispatch_semaphore_t inFlightSemaphore;


@property(nonatomic, retain) id<MTLFunction> modelVertexProgram;
@property(nonatomic, retain) id<MTLFunction> modelFragmentProgram;

@property(nonatomic, retain) id<MTLFunction> lineVertexProgram;
@property(nonatomic, retain) id<MTLFunction> lineFragmentProgram;

@property(nonatomic, assign) SDL_Renderer* currentRenderer;
@property(nonatomic, assign) SDL_Window* currentWindow;
@end


class Bitmap;
MetalRendererHelper* gRenderHelper;

// The max number of command buffers in flight
static const uint32_t TVMaxBuffersInFlight = 3;


@implementation MetalRendererHelper
{
   dispatch_semaphore_t _inFlightSemaphore;

   // The device (aka GPU) we're using to render
   id<MTLDevice> _device;

   // Our render pipeline composed of our vertex and fragment shaders in the .metal shader file
   id<MTLRenderPipelineState> _pipelineState;

   // The command Queue from which we'll obtain command buffers
   id<MTLCommandQueue> _commandQueue;

   id<MTLCommandBuffer> _currentCommandBuffer;
   id<MTLRenderCommandEncoder> _currentCommandEncoder;
   id <CAMetalDrawable> _currentDrawable;
   NSUInteger _currentBufferIdx;
   id<MTLRenderCommandEncoder> _currentRenderEncoder;

   // The current size of our view so we can use this in our render pipeline
   vector_uint2 _viewportSize;
   vector_uint2 _depthSize;

   CAMetalLayer* _layer;

   id<MTLFunction> _modelVertexProgram;
   id<MTLFunction> _modelFragmentProgram;

   id<MTLFunction> _lineVertexProgram;
   id<MTLFunction> _lineFragmentProgram;

   NSArray* _uniformBuffers;
   NSMutableArray<id<MTLRenderPipelineState>>* _modelPipelineStates;
   id<MTLDepthStencilState> _modelDepthState;
   id<MTLDepthStencilState> _lineDepthState;
   id<MTLRenderPipelineState> _linePipelineState;
   id<MTLSamplerState> _modelSamplerState;
   
   id<MTLTexture> _defaultTexture;
   
   NSMutableArray<id<MTLTexture>>* _modelDepthBuffers;
   
   NSMutableArray* _textures;
   NSMutableArray* _vertexBuffers;
   NSMutableArray* _texBuffers;
   NSMutableArray* _indBuffers;
   
   slm::mat4 _currentProjection;
   slm::mat4 _currentInvProjection;
   slm::mat4 _currentModel;
   slm::mat4 _currentView;
   
   slm::vec3 _lightPos;
   slm::vec4 _lightColor;
   
   float _alphaTestVal;
   uint32_t _currentModelIdx;

   SDL_Renderer* _currentRenderer;
   SDL_Window* _currentWindow;
}

- (id<MTLRenderPipelineState>)createModelPipelineState:(ModelPipelineState)state withPixelFormat:(MTLPixelFormat)pixelFormat
{
   
   // Configure a pipeline descriptor that is used to create a pipeline state
   MTLRenderPipelineDescriptor *pipelineStateDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
   pipelineStateDescriptor.label = [NSString stringWithFormat:@"Model Pipeline %i", (int)pixelFormat];
   pipelineStateDescriptor.vertexFunction = _modelVertexProgram;
   pipelineStateDescriptor.fragmentFunction = _modelFragmentProgram;
   pipelineStateDescriptor.colorAttachments[0].pixelFormat = pixelFormat;
   
   switch (state)
   {
      case ModelPipeline_DefaultDiffuse:
         break;
      case ModelPipeline_AdditiveBlend:
         pipelineStateDescriptor.colorAttachments[0].blendingEnabled = YES;
         pipelineStateDescriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
         pipelineStateDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
         pipelineStateDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
         break;
      case ModelPipeline_SubtractiveBlend:
         pipelineStateDescriptor.colorAttachments[0].blendingEnabled = YES;
         pipelineStateDescriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
         pipelineStateDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorZero;
         pipelineStateDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceColor;
         break;
      case ModelPipeline_TranslucentBlend:
         pipelineStateDescriptor.colorAttachments[0].blendingEnabled = YES;
         pipelineStateDescriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
         pipelineStateDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
         pipelineStateDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
         break;
   }
   
   pipelineStateDescriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
   
   NSError* error = NULL;
   id<MTLRenderPipelineState> pipelineState = [_device newRenderPipelineStateWithDescriptor:pipelineStateDescriptor
                                                                                                error:&error];
   if (!pipelineState)
   {
      // Pipeline State creation could fail if we haven't properly set up our pipeline descriptor.
      //  If the Metal API validation is enabled, we can find out more information about what
      //  went wrong.  (Metal API validation is enabled by default when a debug build is run
      //  from Xcode)
      NSLog(@"Failed to created pipeline state, error %@", error);
      return nil;
   }
   
   return pipelineState;
}

- (id<MTLRenderPipelineState>)createLinePipelineStateWithPixelFormat:(MTLPixelFormat)pixelFormat
{
   // Configure a pipeline descriptor that is used to create a pipeline state
   MTLRenderPipelineDescriptor *pipelineStateDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
   pipelineStateDescriptor.label = @"Line Pipeline";
   pipelineStateDescriptor.vertexFunction = _lineVertexProgram;
   pipelineStateDescriptor.fragmentFunction = _lineFragmentProgram;
   pipelineStateDescriptor.colorAttachments[0].pixelFormat = pixelFormat;
   
   // Need this even though we dont write to depth
   pipelineStateDescriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
   
   NSError* error = NULL;
   id<MTLRenderPipelineState> pipelineState = [_device newRenderPipelineStateWithDescriptor:pipelineStateDescriptor error:&error];
   if (!pipelineState)
   {
      // Pipeline State creation could fail if we haven't properly set up our pipeline descriptor.
      //  If the Metal API validation is enabled, we can find out more information about what
      //  went wrong.  (Metal API validation is enabled by default when a debug build is run
      //  from Xcode)
      NSLog(@"Failed to created pipeline state, error %@", error);
      return nil;
   }
   
   return pipelineState;
}

- (void)handleResize
{
   int x,y;
   SDL_GetRendererOutputSize(_currentRenderer, &x, &y);
   
   _viewportSize = (vector_uint2){(uint32_t)x,(uint32_t)y};
   _depthSize = (vector_uint2){0,0};
   
   printf("viewport size now %u,%u\n", x,y);
}

- (void)setup
{
   assert(sizeof(LineVertex) == sizeof(_LineVert));
   _inFlightSemaphore = dispatch_semaphore_create(TVMaxBuffersInFlight);
   _currentBufferIdx = 0;
   int x,y;
   SDL_GetRendererOutputSize(_currentRenderer, &x, &y);
   
   _viewportSize = (vector_uint2){(uint32_t)x,(uint32_t)y};
   _depthSize = (vector_uint2){0,0};
   
   CAMetalLayer* layer = (__bridge CAMetalLayer*)(SDL_RenderGetMetalLayer(_currentRenderer));
   _device = layer.device;
   _layer = layer;
   
   id<MTLBuffer> uniformBuffersCArray[TVMaxBuffersInFlight];
   for(NSUInteger i = 0; i < TVMaxBuffersInFlight; i++)
   {
      // Indicate shared storage so that both the  CPU can access the buffers
      const MTLResourceOptions storageMode = MTLResourceStorageModeShared;
      
      uniformBuffersCArray[i] = [_device newBufferWithLength:sizeof(TVUniforms)
                                                                  options:storageMode];
      
      uniformBuffersCArray[i].label = [NSString stringWithFormat:@"UniformBuffer%lu", i];
   }
   
   _uniformBuffers = [[NSArray alloc] initWithObjects:uniformBuffersCArray count:TVMaxBuffersInFlight];
   
   // Load all the shader files with a .metal file extension in the project
   id<MTLLibrary> defaultLibrary = [_device newDefaultLibrary];
   
   // Load the vertex function from the library
   id<MTLFunction> vertexFunction = [defaultLibrary newFunctionWithName:@"vertexShader"];
   
   // Load the fragment function from the library
   id<MTLFunction> fragmentFunction = [defaultLibrary newFunctionWithName:@"fragmentShader"];
   
   // Configure a pipeline descriptor that is used to create a pipeline state
   MTLRenderPipelineDescriptor *pipelineStateDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
   pipelineStateDescriptor.label = @"Simple Pipeline";
   pipelineStateDescriptor.vertexFunction = vertexFunction;
   pipelineStateDescriptor.fragmentFunction = fragmentFunction;
   pipelineStateDescriptor.colorAttachments[0].pixelFormat = layer.pixelFormat;
   
   NSError* error = NULL;
   _pipelineState = [_device newRenderPipelineStateWithDescriptor:pipelineStateDescriptor
                                                                                      error:&error];
   if (!_pipelineState)
   {
      // Pipeline State creation could fail if we haven't properly set up our pipeline descriptor.
      //  If the Metal API validation is enabled, we can find out more information about what
      //  went wrong.  (Metal API validation is enabled by default when a debug build is run
      //  from Xcode)
      NSLog(@"Failed to created pipeline state, error %@", error);
      return;
   }
   
   _modelVertexProgram = [defaultLibrary newFunctionWithName:@"modelVertexShader"];
   _modelFragmentProgram = [defaultLibrary newFunctionWithName:@"modelFragmentShader"];
   _lineVertexProgram = [defaultLibrary newFunctionWithName:@"lineVertexShader"];
   _lineFragmentProgram = [defaultLibrary newFunctionWithName:@"lineFragmentShader"];
   
   // Model states
   _modelPipelineStates = [NSMutableArray arrayWithCapacity:4];
   
   MTLDepthStencilDescriptor* depthStateDesc = [[MTLDepthStencilDescriptor alloc] init];
   depthStateDesc.depthCompareFunction = MTLCompareFunctionLess;
   depthStateDesc.depthWriteEnabled = YES;
   _modelDepthState = [_device newDepthStencilStateWithDescriptor:depthStateDesc];
   
   depthStateDesc.depthCompareFunction = MTLCompareFunctionAlways;
   depthStateDesc.depthWriteEnabled = NO;
   _lineDepthState = [_device newDepthStencilStateWithDescriptor:depthStateDesc];
   
   
   MTLSamplerDescriptor *samplerDescriptor = [MTLSamplerDescriptor new];
   samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
   samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
   samplerDescriptor.sAddressMode = MTLSamplerAddressModeRepeat;
   samplerDescriptor.tAddressMode = MTLSamplerAddressModeRepeat;
   samplerDescriptor.normalizedCoordinates = YES;
   
   _modelSamplerState = [_device newSamplerStateWithDescriptor:samplerDescriptor];
   
   [_modelPipelineStates addObject:[self createModelPipelineState:ModelPipeline_DefaultDiffuse   withPixelFormat:layer.pixelFormat]];
   [_modelPipelineStates addObject:[self createModelPipelineState:ModelPipeline_AdditiveBlend    withPixelFormat:layer.pixelFormat]];
   [_modelPipelineStates addObject:[self createModelPipelineState:ModelPipeline_SubtractiveBlend withPixelFormat:layer.pixelFormat]];
   [_modelPipelineStates addObject:[self createModelPipelineState:ModelPipeline_TranslucentBlend withPixelFormat:layer.pixelFormat]];
   _linePipelineState = [self createLinePipelineStateWithPixelFormat:layer.pixelFormat];

   _textures = [NSMutableArray arrayWithCapacity:10];
   
   _vertexBuffers = [NSMutableArray arrayWithCapacity:10];
   _texBuffers = [NSMutableArray arrayWithCapacity:10];
   _indBuffers = [NSMutableArray arrayWithCapacity:10];
   
   // Default texture
   MTLTextureDescriptor *textureDescriptor = [[MTLTextureDescriptor alloc] init];
   uint8_t texData[64*64*4];
   memset(texData, '\0', sizeof(texData));
   textureDescriptor.width = 64;
   textureDescriptor.height = 64;
   _defaultTexture = [_device newTextureWithDescriptor:textureDescriptor];
   if (_defaultTexture)
   {
      MTLRegion region = {
         { 0, 0, 0 },      // MTLOrigin
         {64, 64, 1} // MTLSize
      };
      [_defaultTexture replaceRegion:region
             mipmapLevel:0
               withBytes:texData
             bytesPerRow:64*4];
   }
   
   // Create the command queue
   _commandQueue = [_device newCommandQueue];
   
   ImGui_ImplMetal_Init(_device);
}

- (bool)beginFrame
{
   dispatch_semaphore_wait(_inFlightSemaphore, DISPATCH_TIME_FOREVER);
   
   CAMetalLayer* layer = _layer;
   
   if (_depthSize.x != _viewportSize.x || _depthSize.y != _viewportSize.y)
   {
      _depthSize = _viewportSize;
      _modelDepthBuffers = [NSMutableArray arrayWithCapacity:TVMaxBuffersInFlight];
      
      MTLTextureDescriptor * depthBufferDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:_depthSize.x height:_depthSize.y mipmapped:NO];
      depthBufferDescriptor.usage = MTLTextureUsageUnknown;
      depthBufferDescriptor.storageMode = MTLStorageModePrivate;
      depthBufferDescriptor.resourceOptions = MTLResourceStorageModePrivate;
      
      for (int i=0; i<3; i++)
      {
         id<MTLTexture> tex = [_device newTextureWithDescriptor:depthBufferDescriptor];
         [_modelDepthBuffers addObject:tex];
      }
   }
   
   // Obtain a renderPassDescriptor generated from the view's drawable textures
   MTLRenderPassDescriptor *renderPassDescriptor = nil;
   _currentDrawable = layer.nextDrawable;
   
   if (_currentDrawable)
   {
      // Create a new command buffer for each render pass to the current drawable
      _currentCommandBuffer = [_commandQueue commandBuffer];
      _currentCommandBuffer.label = @"FrameCmdBuffer";
      
      renderPassDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
      renderPassDescriptor.colorAttachments[0].texture = _currentDrawable.texture;
      renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
      renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 1.0, 1.0, 1.0);
      renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
      
      renderPassDescriptor.depthAttachment.texture = [_modelDepthBuffers objectAtIndex:_currentBufferIdx];
      renderPassDescriptor.depthAttachment.loadAction = MTLLoadActionClear;
      renderPassDescriptor.depthAttachment.storeAction = MTLStoreActionDontCare;
      renderPassDescriptor.depthAttachment.clearDepth = 1.0;
      
      // Create a render command encoder so we can render into something
      _currentRenderEncoder = [_currentCommandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
      _currentRenderEncoder.label = @"MyRenderEncoder";
      
      [_currentRenderEncoder setViewport:(MTLViewport){0.0, 0.0, (double)_viewportSize.x, (double)_viewportSize.y, -1.0, 1.0 }];
      
      
      ImGui_ImplMetal_NewFrame(renderPassDescriptor);
      ImGui_ImplSDL2_NewFrame(_currentWindow);
      ImGui::NewFrame();
      
      return YES;
   }
   
   return NO;
}

- (void)testDraw:(slm::vec3)pos
{
   static const TVVertex triangleVertices[] =
   {
      // 2D positions,    RGBA colors
      { {  250,  -250 }, { 1, 0, 0, 1 } },
      { { -250,  -250 }, { 0, 1, 0, 1 } },
      { {    0,   250 }, { 0, 0, 1, 1 } },
   };
   
   id<MTLBuffer> uniformBuffer = [_uniformBuffers objectAtIndex:_currentBufferIdx];
   _currentBufferIdx += 1;
   _currentBufferIdx = _currentBufferIdx % TVMaxBuffersInFlight;
   
   TVUniforms *uniforms = (TVUniforms*)uniformBuffer.contents;
   uniforms->viewport_size = _viewportSize;
   
   slm::mat4 modelXfm = slm::translation(pos);
   assert(sizeof(uniforms->model_matrix) == sizeof(slm::mat4));
   memcpy(&uniforms->view_matrix, modelXfm.begin(), sizeof(modelXfm));
   
   vector_uint2 viewportSize = _viewportSize;
   // Set the region of the drawable to which we'll draw.
   [_currentRenderEncoder setViewport:(MTLViewport){0.0, 0.0, (double)viewportSize.x, (double)viewportSize.y, -1.0, 1.0 }];
   
   [_currentRenderEncoder setRenderPipelineState:_pipelineState];
   
   // We call -[MTLRenderCommandEncoder setVertexBytes:length:atIndex:] to send data from our
   //   Application ObjC code here to our Metal 'vertexShader' function
   // This call has 3 arguments
   //   1) A pointer to the memory we want to pass to our shader
   //   2) The memory size of the data we want passed down
   //   3) An integer index which corresponds to the index of the buffer attribute qualifier
   //      of the argument in our 'vertexShader' function
   
   // You send a pointer to the `triangleVertices` array also and indicate its size
   // The `TVVertexInputIndexVertices` enum value corresponds to the `vertexArray`
   // argument in the `vertexShader` function because its buffer attribute also uses
   // the `TVVertexInputIndexVertices` enum value for its index
   [_currentRenderEncoder setVertexBytes:triangleVertices
                          length:sizeof(triangleVertices)
                         atIndex:TVBufferIndexMeshPositions];
   
   
   // You send a pointer to `_viewportSize` and also indicate its size
   // The `TVVertexInputIndexViewportSize` enum value corresponds to the
   // `viewportSizePointer` argument in the `vertexShader` function because its
   //  buffer attribute also uses the `TVVertexInputIndexViewportSize` enum value
   //  for its index
   [_currentRenderEncoder setVertexBuffer:uniformBuffer offset:0 atIndex:TVBufferIndexUniforms];
   
   // Draw the 3 vertices of our triangle
   [_currentRenderEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                             vertexStart:0
                             vertexCount:3];
   
}

- (void)endFrame
{
   _currentBufferIdx += 1;
   _currentBufferIdx = _currentBufferIdx % TVMaxBuffersInFlight;
   __block dispatch_semaphore_t block_sema = _inFlightSemaphore;
   [_currentCommandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer)
    {
       dispatch_semaphore_signal(block_sema);
    }];
   
   ImGui::Render();
   ImDrawData *drawData = ImGui::GetDrawData();
   ImGui_ImplMetal_RenderDrawData(drawData, _currentCommandBuffer, _currentRenderEncoder);
   
   [_currentRenderEncoder endEncoding];
   
   // Schedule a present once the framebuffer is complete using the current drawable
   [_currentCommandBuffer presentDrawable:_currentDrawable];
   [_currentCommandBuffer commit];
   
   _currentCommandBuffer = nil;
   _currentCommandEncoder = nil;
   _currentDrawable = nil;
   _currentRenderEncoder = nil;
   
}

- (void)beginLinePipelineState
{
   [_currentRenderEncoder setRenderPipelineState:_linePipelineState];
   [_currentRenderEncoder setDepthStencilState:_lineDepthState];
   [_currentRenderEncoder setCullMode:MTLCullModeNone];
}

- (void)drawLineStart:(slm::vec3)start end:(slm::vec3)end color:(slm::vec4)color width:(float)width
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
   
   TVUniforms lineUniforms;
   memcpy(&lineUniforms.projection_matrix, &_currentProjection, sizeof(slm::mat4));
   memcpy(&lineUniforms.projection_matrix_inverse, &_currentInvProjection, sizeof(slm::mat4));
   memcpy(&lineUniforms.model_matrix, &_currentModel, sizeof(slm::mat4));
   memcpy(&lineUniforms.view_matrix, &_currentView, sizeof(slm::mat4));
   lineUniforms.viewport_size = _viewportSize;
   lineUniforms.line_params.x = 1.0f/_viewportSize.x;
   lineUniforms.line_params.y = 1.0f/_viewportSize.y;
   lineUniforms.line_params.z = width;
   lineUniforms.line_params.w = 1.0f;
   
   [_currentRenderEncoder setVertexBytes:verts
                                  length:sizeof(verts)
                                 atIndex:TVBufferIndexMeshPositions];
   
   [_currentRenderEncoder setVertexBytes:&lineUniforms
                            length:sizeof(TVUniforms)
                           atIndex:TVBufferIndexUniforms];
   
   [_currentRenderEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

- (void)setModel:(slm::mat4&)model View:(slm::mat4&)view Projection:(slm::mat4&)proj
{
   slm::mat4 correctMat(1);
   correctMat[0] = slm::vec4(1,0,0,0);
   correctMat[1] = slm::vec4(0,1,0,0);
   correctMat[2] = slm::vec4(0,0,0.5,0);
   correctMat[3] = slm::vec4(0,0,0.5,1);
   _currentProjection = proj * correctMat;
   _currentInvProjection = slm::inverse(proj);
   _currentModel = model;
   _currentView = view;
}

- (int32_t)loadTexture:(Bitmap*)bmp defaultPalette:(Palette*)defaultPal
{
   MTLTextureDescriptor *textureDescriptor = [[MTLTextureDescriptor alloc] init];
   
   uint8_t* texData = NULL;
   uint32_t pow2W = getNextPow2(bmp->mWidth);
   uint32_t pow2H = getNextPow2(bmp->mHeight);
   
   textureDescriptor.width = pow2W;
   textureDescriptor.height = pow2H;
   
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
         texData = new uint8_t[pow2W*pow2H*4];
         copyMipRGBA(bmp->mWidth, bmp->mHeight, pow2W*4, pal, bmp->mMips[0], texData, 255);
         textureDescriptor.pixelFormat = bmp->mBGR ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
      }
      else if (bmp->mFlags & Bitmap::FLAG_TRANSLUCENT)
      {
         texData = new uint8_t[pow2W*pow2H*4];
         copyMipRGBA(bmp->mWidth, bmp->mHeight, pow2W*4, pal, bmp->mMips[0], texData, 1);
         textureDescriptor.pixelFormat = bmp->mBGR ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
      }
      else
      {
         texData = new uint8_t[pow2W*pow2H*4];
         copyMipRGBA(bmp->mWidth, bmp->mHeight, pow2W*4, pal, bmp->mMips[0], texData, 256);
         textureDescriptor.pixelFormat = bmp->mBGR ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
      }
   }
   else if (bmp->mBitDepth == 24)
   {
      uint8_t* texData = new uint8_t[pow2W*pow2H*4];
      copyMipDirectPadded(bmp->mHeight, bmp->getStride(bmp->mWidth), pow2W*3, bmp->mMips[0], texData);
      textureDescriptor.pixelFormat = bmp->mBGR ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
   }
   else
   {
      assert(false);
      return -1;
   }
   
   id<MTLTexture> tex = [_device newTextureWithDescriptor:textureDescriptor];
   if (tex)
   {
      MTLRegion region = {
         { 0, 0, 0 },      // MTLOrigin
         {pow2W, pow2H, 1} // MTLSize
      };
      [tex replaceRegion:region
                  mipmapLevel:0
                    withBytes:texData
                  bytesPerRow:pow2W*4];
      delete[] texData;
      int sz = [_textures count];
      for (int i=0; i<sz; i++)
      {
         NSObject* obj = [_textures objectAtIndex:i];
         if ([[obj className] isEqualToString:@"NSNull"])
         {
            [_textures setObject:tex atIndexedSubscript:i];
            return i;
         };
      }
      [_textures addObject:tex];
      return (uint32_t)[_textures count]-1;
   }
   else
   {
      delete[] texData;
      return -1;
   }
}

- (void)deleteTexture:(int32_t)texID
{
   if (texID < 0 || texID >= [_textures count])
      return;
   
   NSObject* obj = [_textures objectAtIndex:texID];
   if ([[obj className] isEqualToString:@"NSNull"])
      return;
   
   [_textures setObject:_defaultTexture atIndexedSubscript:texID];
}

- (void)loadModelData:(uint32)modelId verts:(void*)verts texverts:(void*)texverts inds:(void*)inds numVerts:(uint32_t)numVerts numTexVerts:(uint32_t)numTexVerts numInds:(uint32_t)numInds
{
   if ([_vertexBuffers count] <= modelId)
   {
      while ([_vertexBuffers count] <= modelId)
         [_vertexBuffers addObject:[NSNull null]];
      while ([_texBuffers count] <= modelId)
         [_texBuffers addObject:[NSNull null]];
      while ([_indBuffers count] <= modelId)
         [_indBuffers addObject:[NSNull null]];
   }
   
   id<MTLBuffer> vertBuffer = [_device newBufferWithBytes:verts length:sizeof(slm::vec3)*numVerts options:
                               MTLResourceCPUCacheModeDefaultCache];
   
   id<MTLBuffer> texBuffer = [_device newBufferWithBytes:texverts length:sizeof(slm::vec2)*numTexVerts options:
                               MTLResourceCPUCacheModeDefaultCache];
   
   id<MTLBuffer> indBuffer = [_device newBufferWithBytes:inds length:sizeof(uint16_t)*numInds options:
                               MTLResourceCPUCacheModeDefaultCache];

   [_vertexBuffers setObject:vertBuffer atIndexedSubscript:modelId];
   [_texBuffers setObject:texBuffer atIndexedSubscript:modelId];
   [_indBuffers setObject:indBuffer atIndexedSubscript:modelId];
}

- (void)setModel:(uint32_t)modelId vertOffset:(uint32_t)vertOffset texOffset:(uint32_t)texOffset
{
   if (modelId >= [_vertexBuffers count])
      return;
   
   id<MTLBuffer> vertBuffer = [_vertexBuffers objectAtIndex:modelId];
   id<MTLBuffer> texBuffer = [_texBuffers objectAtIndex:modelId];
   
   [_currentRenderEncoder setVertexBuffer:vertBuffer offset:vertOffset*sizeof(slm::vec3)*2 atIndex:TVBufferIndexMeshPositions];
   [_currentRenderEncoder setVertexBuffer:texBuffer offset:texOffset*sizeof(slm::vec2) atIndex:TVBufferIndexMeshTexcoords];
   
   _currentModelIdx = modelId;
}


- (void)setLightPos:(slm::vec3)pos color:(slm::vec4)ambient
{
   _lightPos = pos;
   _lightColor = ambient;
}

- (void)beginModelPipelineState:(ModelPipelineState)state texID:(int32_t)texID alphaTestVal:(float)testVal
{
   [_currentRenderEncoder setRenderPipelineState:[_modelPipelineStates objectAtIndex:state]];
   [_currentRenderEncoder setDepthStencilState:_modelDepthState];
   [_currentRenderEncoder setFrontFacingWinding:MTLWindingClockwise];
   [_currentRenderEncoder setCullMode:MTLCullModeBack];
   [_currentRenderEncoder setFragmentTexture:[_textures objectAtIndex:texID] atIndex:TVTextureDiffuse];
   _alphaTestVal = testVal;
}


- (void)drawModelVerts:(uint32_t)numVerts inds:(uint32_t)numInds startInd:(uint32_t)startInds startVert:(uint32_t)startVerts
{
   TVUniforms modelUniforms;
   memcpy(&modelUniforms.projection_matrix, &_currentProjection, sizeof(slm::mat4));
   memcpy(&modelUniforms.projection_matrix_inverse, &_currentInvProjection, sizeof(slm::mat4));
   memcpy(&modelUniforms.model_matrix, &_currentModel, sizeof(slm::mat4));
   memcpy(&modelUniforms.view_matrix, &_currentView, sizeof(slm::mat4));
   modelUniforms.viewport_size = _viewportSize;
   modelUniforms.alpha_test = _alphaTestVal;
   
   id<MTLBuffer> indBuffer = [_indBuffers objectAtIndex:_currentModelIdx];
   
   [_currentRenderEncoder setFragmentSamplerState:_modelSamplerState atIndex:0];
   
   [_currentRenderEncoder setVertexBytes:&modelUniforms
                                  length:sizeof(TVUniforms)
                                 atIndex:TVBufferIndexUniforms];
   
   [_currentRenderEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                     indexCount:numInds
                                      indexType:MTLIndexTypeUInt16
                                    indexBuffer:indBuffer
                              indexBufferOffset:startInds*2
                                  instanceCount:1
                                    baseVertex:startVerts
                                    baseInstance:0];
}

@end

int32_t GFXLoadTexture(Bitmap* bmp, Palette*pal)
{
   return [gRenderHelper loadTexture:bmp defaultPalette:pal];
}

void GFXDeleteTexture(int32_t texID)
{
   [gRenderHelper deleteTexture:texID];
}

void GFXLoadModelData(uint32_t modelId, void* verts, void* texverts, void* inds, uint32_t numVerts, uint32_t numTexVerts, uint32_t numInds)
{
   [gRenderHelper loadModelData:modelId verts:verts texverts:texverts inds:inds numVerts:numVerts numTexVerts:numTexVerts numInds:numInds];
}

void GFXSetModelViewProjection(slm::mat4 &model, slm::mat4 &view, slm::mat4 &proj)
{
   [gRenderHelper setModel:model View:view Projection:proj];
}

void GFXSetLightPos(slm::vec3 pos, slm::vec4 ambient)
{
   [gRenderHelper setLightPos:pos color:ambient];
}

void GFXBeginModelPipelineState(ModelPipelineState state, int32_t texID, float testVal)
{
   [gRenderHelper beginModelPipelineState:state texID:texID alphaTestVal:testVal];
}

void GFXSetModelVerts(uint32_t modelId, uint32_t vertOffset, uint32_t texOffset)
{
   [gRenderHelper setModel:modelId vertOffset:vertOffset texOffset:texOffset];
}

void GFXDrawModelPrims(uint32_t numVerts, uint32_t numInds, uint32_t startInds, uint32_t startVerts)
{
   [gRenderHelper drawModelVerts:numVerts inds:numInds startInd:startInds startVert:startVerts];
}

void GFXBeginLinePipelineState()
{
   [gRenderHelper beginLinePipelineState];
}

void GFXDrawLine(slm::vec3 start, slm::vec3 end, slm::vec4 color, float width)
{
   [gRenderHelper drawLineStart:start end:end color:color width:width];
}

bool GFXSetup(SDL_Window* window, SDL_Renderer* renderer)
{
   int metalDriverIdx = -1;
   int drivers = SDL_GetNumRenderDrivers();
   for (int i=0; i<drivers; i++)
   {
      SDL_RendererInfo info;
      SDL_GetRenderDriverInfo(i, &info);
      
      if (strcasecmp(info.name, "metal") == 0)
      {
         metalDriverIdx = i;
      }
      //printf("Render driver[%i] == %s\n", i, info.name);
   }
   
   if (metalDriverIdx == -1)
   {
      fprintf(stderr, "Unable to find metal render driver for SDL\n");
      return false;
   }
   
   //SDL_Renderer* renderer = SDL_CreateRenderer(window, metalDriverIdx, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
   
   SDL_SetRenderDrawColor(renderer, 255, 0, 255, 0);
   SDL_RenderClear(renderer);
   SDL_RenderPresent(renderer);
   
   // Init gui
   IMGUI_CHECKVERSION();
   ImGui::CreateContext();
   ImGuiIO& io = ImGui::GetIO(); (void)io;
   
   ImGui_ImplSDL2_InitForOpenGL(window, NULL);
   
   gRenderHelper = [[MetalRendererHelper alloc] init];
   gRenderHelper.currentRenderer = renderer;
   gRenderHelper.currentWindow = window;
   
   [gRenderHelper setup];
   
   return true;
}

bool GFXBeginFrame()
{
   return [gRenderHelper beginFrame];
}

void GFXEndFrame()
{
   return [gRenderHelper endFrame];
}

void GFXHandleResize()
{
   [gRenderHelper handleResize];
}

void GFXTestRender(slm::vec3 pos)
{
   if ([gRenderHelper beginFrame])
   {
      [gRenderHelper testDraw:pos];
      [gRenderHelper endFrame];
   }
}

#endif


