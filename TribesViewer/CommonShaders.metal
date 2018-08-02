#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

// Include header shared between this Metal shader code and C code executing Metal API commands
#import "CommonShaderTypes.h"

// Vertex shader outputs and fragment shader inputs
typedef struct
{
   float4 clipSpacePosition [[position]];
   float4 color;
   float2 uv;
   float alpha_test;
} RasterizerData;

// Vertex function
vertex RasterizerData
vertexShader(uint vertexID [[vertex_id]],
             constant TVUniforms &uniforms [[ buffer(TVBufferIndexUniforms) ]],
             device TVVertex *vertices [[buffer(TVBufferIndexMeshPositions)]])
{
   RasterizerData out;
   
   // Initialize our output clip space position
   out.clipSpacePosition = vector_float4(0.0, 0.0, 0.0, 1.0);
   
   // Index into our array of positions to get the current vertex
   //   Our positions are specified in pixel dimensions (i.e. a value of 100 is 100 pixels from
   //   the origin)
   float2 pixelSpacePosition = vertices[vertexID].position.xy;
   
   float4 realPos = uniforms.view_matrix * float4(pixelSpacePosition.x, pixelSpacePosition.y, 0, 1);
   
   pixelSpacePosition.x = realPos.x;
   pixelSpacePosition.y = realPos.y;
   
   // Dereference viewportSizePointer and cast to float so we can do floating-point division
   vector_float2 viewportSize = vector_float2(uniforms.viewport_size);
   
   // The output position of every vertex shader is in clip-space (also known as normalized device
   //   coordinate space, or NDC).   A value of (-1.0, -1.0) in clip-space represents the
   //   lower-left corner of the viewport whereas (1.0, 1.0) represents the upper-right corner of
   //   the viewport.
   
   // Calculate and write x and y values to our clip-space position.  In order to convert from
   //   positions in pixel space to positions in clip-space, we divide the pixel coordinates by
   //   half the size of the viewport.
   out.clipSpacePosition.xy = pixelSpacePosition / (viewportSize / 2.0);
   
   // Pass our input color straight to our output color.  This value will be interpolated
   //   with the other color values of the vertices that make up the triangle to produce
   //   the color value for each fragment in our fragment shader
   out.color = vertices[vertexID].color;
   
   return out;
}

// Fragment function
fragment float4 fragmentShader(RasterizerData in [[stage_in]])
{
   // We return the color we just set which will be written to our color attachment.
   return in.color;
}

fragment float4 modelFragmentShader(RasterizerData in [[stage_in]],
                                    texture2d<half, access::sample> colorTexture [[ texture(TVTextureDiffuse) ]],
                                    sampler linearSampler [[ sampler(0) ]])
{
   // We return the color we just set which will be written to our color attachment.
   float4 col = float4(colorTexture.sample(linearSampler, in.uv));
   if (col.a > in.alpha_test)
      discard_fragment();
   col.r = col.r * in.color.r * in.color.a;
   col.g = col.g * in.color.g * in.color.a;
   col.b = col.b * in.color.b * in.color.a;
   return col;
}

fragment float4 lineFragmentShader(RasterizerData in [[stage_in]])
{
   // We return the color we just set which will be written to our color attachment.
   return in.color;
}

vertex RasterizerData
modelVertexShader(uint vertexID [[vertex_id]],
             constant TVUniforms &uniforms [[ buffer(TVBufferIndexUniforms) ]],
             device ModelVertex *vertices [[buffer(TVBufferIndexMeshPositions)]],
             device ModelTexVertex *texVertices [[buffer(TVBufferIndexMeshTexcoords)]])
{
   RasterizerData out;
   matrix_float4x4 xfmMat = uniforms.view_matrix * uniforms.model_matrix;
   
   matrix_float3x3 rotM = matrix_float3x3(xfmMat[0].xyz, xfmMat[1].xyz, xfmMat[2].xyz);
   vector_float3 normal = normalize(rotM * vector_float3(vertices[vertexID].normal));
   vector_float3 lightDir = normalize(uniforms.light_pos);
   
   float NdotL = max(dot(normal, lightDir), 0.0);
   vector_float4 diffuse = uniforms.light_color;
   
   // Initialize our output clip space position
   matrix_float4x4 xfmMVP = uniforms.projection_matrix * xfmMat;
   out.clipSpacePosition = xfmMVP * vector_float4(vertices[vertexID].position, 1.0);
   out.uv = texVertices[vertexID].texcoord;
   //out.uv.y = 1.0-out.uv.y;
   out.color = vector_float4(1,1,1,1);//NdotL * diffuse;
   out.color.a = 1.0;
   out.alpha_test = uniforms.alpha_test;
   
   return out;
}

vertex RasterizerData
lineVertexShader(uint vertexID [[vertex_id]],
             constant TVUniforms &uniforms [[ buffer(TVBufferIndexUniforms) ]],
             constant LineVertex *vertices [[buffer(TVBufferIndexMeshPositions)]])
{
   RasterizerData out;
   
   
   matrix_float4x4 xfmMat = uniforms.view_matrix;// * uniforms.model_matrix;
   vector_float4 startPos = xfmMat * vector_float4(vertices[vertexID].position, 1);
   vector_float4 endPos = xfmMat * vector_float4(vertices[vertexID].nextPosition, 1);
   
   vector_float4 projStartPos = uniforms.projection_matrix * startPos;
   vector_float4 projEndPos = uniforms.projection_matrix * endPos;
   
   vector_float4 dp = projEndPos - projStartPos;
   vector_float4 delta = normalize(vector_float4(dp.x, dp.y, 0.0, 0.0));
   delta = vector_float4(-delta.y, delta.x, 0, 0);
   vector_float4 realDelta = vector_float4(0,0,0,0);
   realDelta += delta * vertices[vertexID].normal[0];
   realDelta = realDelta * uniforms.line_params.z;
   
   out.clipSpacePosition = ((uniforms.projection_matrix * xfmMat) * vector_float4(vertices[vertexID].position, 1));
   out.clipSpacePosition.xyz /= out.clipSpacePosition.w;
   out.clipSpacePosition += vector_float4((realDelta.xy * uniforms.line_params.xy), 0, 0);
   out.clipSpacePosition.w = out.clipSpacePosition.z = 1;
   
   out.color = vertices[vertexID].color;
   
   return out;
}


