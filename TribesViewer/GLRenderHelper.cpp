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

#ifdef USE_OPENGL

#include <stdint.h>
#include "SDL.h"
#include <stdio.h>
#include <strings.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>
#include <unordered_map>
#include <slm/slmath.h>
#include <GL/gl3w.h>

#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"

#ifdef HAVE_OPENGLES2
#define GL_GLEXT_PROTOTYPES
#include "SDL_opengles2.h"
#else
#include "SDL_opengl.h"
#endif

#include "CommonShaderTypes.h"
#include "CommonData.h"



enum ShaderAttribs
{
   kVertexAttrib_Position,
   kVertexAttrib_Position_Normals,
   kVertexAttrib_Position_TexCoords,
   kVertexAttrib_Position_Color,
   kVertexAttrib_Position_Next,
   kVertexAttrib_COUNT
};

enum ShaderUniforms
{
   kUniform_MVPMatrix,
   kUniform_MVMatrix,
   kUniform_LightPos,
   kUniform_LightColor,
   kUniform_AlphaTestVal,
   kUniform_SamplerID,
   kUniform_COUNT
};

enum LineShaderUniforms
{
   kLineUniform_PMatrix,
   kLineUniform_MVMatrix,
   kLineUniform_Width,
   kLineUniform_ViewportScale,
   kLineUniform_COUNT
};

struct ProgramInfo
{
   GLuint programID;
   GLint uniformLocations[kUniform_COUNT];
   ProgramInfo() : programID(0) { memset(uniformLocations, '\0', sizeof(uniformLocations)); }
};

struct LineProgramInfo
{
   GLuint programID;
   GLint uniformLocations[kLineUniform_COUNT];
   LineProgramInfo() : programID(0) { memset(uniformLocations, '\0', sizeof(uniformLocations)); }
};

const char* sStandardFragmentProgram = "#version 330 core\n\
\n\
in vec2 vTexCoord0;\n\
in vec4 vColor0;\n\
uniform sampler2D texture0;\n\
uniform float alphaTestF;\n\
out vec4 Color;\n\
\n\
void main()\n\
{\n\
Color = texture(texture0, vTexCoord0);\n\
if (Color.a > alphaTestF) discard;\n\
Color.r = Color.r * vColor0.r * vColor0.a;\n\
Color.g = Color.g * vColor0.g * vColor0.a;\n\
Color.b = Color.b * vColor0.b * vColor0.a;\n\
}\n\
";

const char* sStandardVertexProgram = "#version 330 core\n\
\n\
layout(location = 0) in vec3 aPosition;\n\
layout(location = 1) in vec3 aNormal;\n\
layout(location = 2) in vec2 aTexCoord0;\n\
\n\
uniform mat4 worldMatrixProjection;\n\
uniform mat4 worldMatrix;\n\
uniform vec3 lightPos;\n\
uniform vec3 lightColor;\n\
\n\
out vec2 vTexCoord0;\n\
out vec4 vColor0;\n\
\n\
void main()\n\
{\n\
vec3 normal, lightDir;\n\
vec4 diffuse;\n\
float NdotL;\n\
\n\
normal = normalize(mat3(worldMatrix) * aNormal);\n\
\n\
lightDir = normalize(vec3(lightPos));\n\
\n\
NdotL = max(dot(normal, lightDir), 0.0);\n\
\n\
diffuse = vec4(lightColor, 1.0);\n\
\n\
gl_Position = worldMatrixProjection * vec4(aPosition,1);\n\
vTexCoord0 = aTexCoord0;\n\
vColor0 = vec4(1,1,1,1);//NdotL * diffuse;\n\
vColor0.a = 1.0;\n\
}\n\
";

const char* sLineFragmentProgram = "#version 330 core\n\
\n\
in vec4 vColor0;\n\
out vec4 Color;\n\
\n\
void main()\n\
{\n\
Color = vColor0;\n\
}\n\
";

const char* sLineVertexProgram = "#version 330 core\n\
\n\
in vec3 aPosition;\n\
in vec3 aNormal;\n\
in vec4 aColor0;\n\
in vec3 aNext;\n\
\n\
uniform mat4 projection;\n\
uniform mat4 worldMatrix;\n\
uniform float lineWidth;\n\
uniform vec2 viewportScale;\n\
\n\
out vec4 vColor0;\n\
\n\
void main()\n\
{\n\
vec4 startPos = worldMatrix * vec4(aPosition, 1);\n\
vec4 endPos = worldMatrix * vec4(aNext, 1);\n\
vec4 projStartPos = projection * startPos;\n\
vec4 projEndPos = projection * endPos;\n\
vec4 dp = projEndPos - projStartPos;\n\
vec4 delta = normalize(vec4(dp.x, dp.y, 0, 0));\n\
delta = vec4(-delta.y, delta.x, 0, 0);\n\
vec4 realDelta = vec4(0,0,0,0);\n\
realDelta += delta * aNormal.x;\n\
realDelta = realDelta * lineWidth;\n\
gl_Position = ((projection * worldMatrix) * (vec4(aPosition, 1)));\n\
gl_Position.xyz /= gl_Position.w;\n\
gl_Position += vec4((realDelta.xy * viewportScale), 0, 0);\n\
gl_Position.w = gl_Position.z = 1;\n\
vColor0 = aColor0;\n\
}\n\
";

static GLuint compileShader(GLuint shaderType, const char* data)
{
   GLuint shader = glCreateShader(shaderType);
   glShaderSource(shader, 1, &data, NULL);
   glCompileShader(shader);
   
   GLint status;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
   if (status == GL_FALSE)
   {
      GLint infoLogLength;
      glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLength);
      GLchar *strInfoLog = new GLchar[infoLogLength + 1];
      glGetShaderInfoLog(shader, infoLogLength, NULL, strInfoLog);
      fprintf(stderr, "Failed to compile shader(%i)...\n%s", shaderType, strInfoLog);
      delete[] strInfoLog;
      return 0;
   }
   
   return shader;
}

const uint32_t kLineVertSize = (sizeof(slm::vec3) * 3) + sizeof(slm::vec4);

static LineProgramInfo buildLineProgram()
{
   LineProgramInfo ret;
   GLuint shaders[2];
   
   shaders[0] = compileShader(GL_VERTEX_SHADER, sLineVertexProgram);
   shaders[1] = compileShader(GL_FRAGMENT_SHADER, sLineFragmentProgram);
   
   ret.programID = glCreateProgram();
   
   glAttachShader(ret.programID, shaders[0]);
   glAttachShader(ret.programID, shaders[1]);
   
   glBindAttribLocation(ret.programID, kVertexAttrib_Position, "aPosition");
   glBindAttribLocation(ret.programID, kVertexAttrib_Position_Normals, "aNormal");
   glBindAttribLocation(ret.programID, kVertexAttrib_Position_Color, "aColor0");
   glBindAttribLocation(ret.programID, kVertexAttrib_Position_Next, "aNext");
   
   glLinkProgram(ret.programID);
   
   GLint status = GL_TRUE;
   glGetProgramiv (ret.programID, GL_LINK_STATUS, &status);
   if (status == GL_FALSE)
   {
      GLint infoLogLength = 0;
      glGetProgramiv(ret.programID, GL_INFO_LOG_LENGTH, &infoLogLength);
      
      if (infoLogLength > 0)
      {
         GLchar *strInfoLog = new GLchar[infoLogLength + 1];
         glGetProgramInfoLog(ret.programID, infoLogLength, NULL, strInfoLog);
         fprintf(stderr, "Failed to link shader...\n%s\n", strInfoLog);
         delete[] strInfoLog;
      }
      glDeleteProgram(ret.programID);
      return ret;
   }
   
   glDetachShader(ret.programID, shaders[0]);
   glDetachShader(ret.programID, shaders[1]);
   
   glUseProgram(ret.programID);
   
   ret.uniformLocations[kLineUniform_PMatrix] = glGetUniformLocation(ret.programID, "projection");
   ret.uniformLocations[kLineUniform_MVMatrix] = glGetUniformLocation(ret.programID, "worldMatrix");
   ret.uniformLocations[kLineUniform_Width] = glGetUniformLocation(ret.programID, "lineWidth");
   ret.uniformLocations[kLineUniform_ViewportScale] = glGetUniformLocation(ret.programID, "viewportScale");
   
   return ret;
}

ProgramInfo buildProgram()
{
   ProgramInfo ret;
   GLuint shaders[2];
   
   shaders[0] = compileShader(GL_VERTEX_SHADER, sStandardVertexProgram);
   shaders[1] = compileShader(GL_FRAGMENT_SHADER, sStandardFragmentProgram);
   
   ret.programID = glCreateProgram();
   
   glAttachShader(ret.programID, shaders[0]);
   glAttachShader(ret.programID, shaders[1]);
   
   glBindAttribLocation(ret.programID, kVertexAttrib_Position, "aPosition");
   glBindAttribLocation(ret.programID, kVertexAttrib_Position_Normals, "aNormal");
   glBindAttribLocation(ret.programID, kVertexAttrib_Position_TexCoords, "aTexCoord0");
   
   glLinkProgram(ret.programID);
   
   GLint status = GL_TRUE;
   glGetProgramiv (ret.programID, GL_LINK_STATUS, &status);
   if (status == GL_FALSE)
   {
      GLint infoLogLength = 0;
      glGetProgramiv(ret.programID, GL_INFO_LOG_LENGTH, &infoLogLength);
      
      if (infoLogLength > 0)
      {
         GLchar *strInfoLog = new GLchar[infoLogLength + 1];
         glGetProgramInfoLog(ret.programID, infoLogLength, NULL, strInfoLog);
         
         fprintf(stderr, "Failed to link shader...\n%s\n", strInfoLog);
         
         delete[] strInfoLog;
      }
      glDeleteProgram(ret.programID);
      return ret;
   }
   
   glDetachShader(ret.programID, shaders[0]);
   glDetachShader(ret.programID, shaders[1]);
   
   glUseProgram(ret.programID);
   
   ret.uniformLocations[kUniform_MVPMatrix] = glGetUniformLocation(ret.programID, "worldMatrixProjection");
   ret.uniformLocations[kUniform_MVMatrix] = glGetUniformLocation(ret.programID, "worldMatrix");
   ret.uniformLocations[kUniform_LightPos] = glGetUniformLocation(ret.programID, "lightPos");
   ret.uniformLocations[kUniform_LightColor] = glGetUniformLocation(ret.programID, "lightColor");
   ret.uniformLocations[kUniform_AlphaTestVal] = glGetUniformLocation(ret.programID, "alphaTestF");
   ret.uniformLocations[kUniform_SamplerID] = glGetUniformLocation(ret.programID, "texture0");
   
   return ret;
}

struct GLState
{
   std::vector<GLuint> vertexBuffers;
   std::vector<GLuint> texVertBuffers;
   std::vector<GLuint> indBuffers;
   std::vector<GLuint> textures;
   
   GLuint lineVertexBuffer;
   LineProgramInfo lineProgram;
   ProgramInfo modelProgram;
   
   slm::mat4 projectionMatrix;
   slm::mat4 modelMatrix;
   slm::mat4 viewMatrix;
   
   slm::vec4 lightColor;
   slm::vec3 lightPos;
   slm::vec2 viewportSize;
   
   GLuint currentProgram;
   
   SDL_Window *window;
};

GLState smState;

bool GFXSetup(SDL_Window* window)
{
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
   SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
   SDL_GL_SetSwapInterval(1);
   
   SDL_GLContext mainContext = SDL_GL_CreateContext(window);
   
   if (gl3wInit()) {
      fprintf(stderr, "failed to initialize OpenGL\n");
      return -1;
   }
   
   // Init gui
   IMGUI_CHECKVERSION();
   ImGui::CreateContext();
   ImGuiIO& io = ImGui::GetIO(); (void)io;
   
   ImGui_ImplSDL2_InitForOpenGL(window, &mainContext);
   ImGui_ImplOpenGL3_Init("#version 150");
   ImGui::StyleColorsDark();
   
   smState.modelProgram = buildProgram();
   smState.lineProgram = buildLineProgram();
   smState.window = window;
   
   smState.currentProgram = smState.modelProgram.programID;
   
   int w, h;
   SDL_GetWindowSize(smState.window, &w, &h);
   smState.viewportSize = slm::vec2(w,h);
   
   glGenBuffers(1, &smState.lineVertexBuffer);
   glBindBuffer(GL_ARRAY_BUFFER, smState.lineVertexBuffer);
   glBufferData(GL_ARRAY_BUFFER, 6 * kLineVertSize, NULL, GL_STREAM_DRAW);
   
   GLuint VertexArrayID;
   glGenVertexArrays(1, &VertexArrayID);
   glBindVertexArray(VertexArrayID);
   
   return true;
}

void GFXTeardown()
{
}

void GFXTestRender(slm::vec3 pos)
{
   
}

bool GFXBeginFrame()
{
   int w, h;
   SDL_GetWindowSize(smState.window, &w, &h);
   
   glViewport(0,0,w,h);
   
   glClearColor(0,1,1,1);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   
   ImGui_ImplOpenGL3_NewFrame();
   ImGui_ImplSDL2_NewFrame(smState.window);
   ImGui::NewFrame();
   
   return true;
}

void GFXEndFrame()
{
   ImGui::Render();
   ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
   SDL_GL_SwapWindow(smState.window);
}

void GFXHandleResize()
{
   int w, h;
   SDL_GetWindowSize(smState.window, &w, &h);
   smState.viewportSize = slm::vec2(w,h);
}


int32_t GFXLoadTexture(Bitmap* bmp, Palette* defaultPal)
{
   uint8_t* texData = NULL;
   uint32_t pow2W = getNextPow2(bmp->mWidth);
   uint32_t pow2H = getNextPow2(bmp->mHeight);
   GLuint pixFormat = 0;
   
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
         pixFormat = bmp->mBGR ? GL_BGRA : GL_RGBA;
      }
      else if (bmp->mFlags & Bitmap::FLAG_TRANSLUCENT)
      {
         texData = new uint8_t[pow2W*pow2H*4];
         copyMipRGBA(bmp->mWidth, bmp->mHeight, pow2W*4, pal, bmp->mMips[0], texData, 1);
         pixFormat = bmp->mBGR ? GL_BGRA : GL_RGBA;
      }
      else
      {
         texData = new uint8_t[pow2W*pow2H*4];
         copyMipRGBA(bmp->mWidth, bmp->mHeight, pow2W*4, pal, bmp->mMips[0], texData, 256);
         pixFormat = bmp->mBGR ? GL_BGRA : GL_RGBA;
      }
   }
   else if (bmp->mBitDepth == 24)
   {
      uint8_t* texData = new uint8_t[pow2W*pow2H*4];
      copyMipDirectPadded(bmp->mHeight, bmp->getStride(bmp->mWidth), pow2W*3, bmp->mMips[0], texData);
      pixFormat = bmp->mBGR ? GL_BGRA : GL_RGBA;
   }
   else
   {
      assert(false);
      return -1;
   }
   
   GLuint tex;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
   
   if (tex)
   {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pow2W, pow2H, 0, pixFormat, GL_UNSIGNED_BYTE, texData);
      delete[] texData;
      int sz = smState.textures.size();
      for (int i=0; i<sz; i++)
      {
         if (smState.textures[i] == (uint32_t)-1)
         {
            smState.textures[i] = tex;
            return i;
         }
      }
      smState.textures.push_back(tex);
      return (uint32_t)(smState.textures.size()-1);
   }
   else
   {
      delete[] texData;
      return -1;
   }
}

void GFXDeleteTexture(int32_t texID)
{
   if (texID < 0 || texID >= smState.textures.size())
      return;
   
   GLuint tex = smState.textures[texID];
   if (tex == (uint32_t)-1)
      return;
   
   glDeleteTextures(1, &tex);
   smState.textures[texID] = (uint32_t)-1;
}

void GFXLoadModelData(uint32_t modelId, void* verts, void* texverts, void* inds, uint32_t numVerts, uint32_t numTexVerts, uint32_t numInds)
{
   while (smState.vertexBuffers.size() <= modelId)
      smState.vertexBuffers.push_back((uint32_t)-1);
   while (smState.texVertBuffers.size() <= modelId)
      smState.texVertBuffers.push_back((uint32_t)-1);
   while (smState.indBuffers.size() <= modelId)
      smState.indBuffers.push_back((uint32_t)-1);
   
   GLuint vertexBuffer = smState.vertexBuffers[modelId];
   GLuint texVertBuffer = smState.texVertBuffers[modelId];
   GLuint indBuffer = smState.indBuffers[modelId];
   if (vertexBuffer == (uint32_t)-1)
   {
      glGenBuffers(1, &vertexBuffer);
      glGenBuffers(1, &texVertBuffer);
      glGenBuffers(1, &indBuffer);
      smState.vertexBuffers[modelId] = vertexBuffer;
      smState.texVertBuffers[modelId] = texVertBuffer;
      smState.indBuffers[modelId] = indBuffer;
   }
   
   glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
   glBufferData(GL_ARRAY_BUFFER, numVerts*sizeof(slm::vec3)*2, verts, GL_STATIC_DRAW);
   
   glBindBuffer(GL_ARRAY_BUFFER, texVertBuffer);
   glBufferData(GL_ARRAY_BUFFER, numTexVerts*sizeof(slm::vec2), texverts, GL_STATIC_DRAW);
   
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indBuffer);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, numInds*2, inds, GL_STATIC_DRAW);
}

void GFXSetModelViewProjection(slm::mat4 &model, slm::mat4 &view, slm::mat4 &proj)
{
   smState.modelMatrix = model;
   smState.projectionMatrix = proj;
   smState.viewMatrix = view;
   
   if (smState.currentProgram == smState.lineProgram.programID) {
      glUniformMatrix4fv(smState.lineProgram.uniformLocations[kLineUniform_PMatrix], 1, GL_FALSE, smState.projectionMatrix.begin());
      slm::mat4 vm = smState.viewMatrix; /* * smState.modelMatrix;*/
      glUniformMatrix4fv(smState.lineProgram.uniformLocations[kLineUniform_MVMatrix], 1, GL_FALSE, vm.begin());
   } else {
      slm::mat4 combined = smState.projectionMatrix * smState.viewMatrix * smState.modelMatrix;
      glUniformMatrix4fv(smState.modelProgram.uniformLocations[kUniform_MVPMatrix], 1, GL_FALSE, combined.begin());
      combined = smState.viewMatrix * smState.modelMatrix;
      glUniformMatrix4fv(smState.modelProgram.uniformLocations[kUniform_MVMatrix], 1, GL_FALSE, combined.begin());
   }
}

void GFXSetLightPos(slm::vec3 pos, slm::vec4 ambient)
{
   smState.lightPos = pos;
   smState.lightColor = ambient;
   
   if (smState.currentProgram == smState.modelProgram.programID) {
      glUniform3fv(smState.modelProgram.uniformLocations[kUniform_LightColor], 1, smState.lightColor.begin());
      glUniform3fv(smState.modelProgram.uniformLocations[kUniform_LightPos], 1, smState.lightPos.begin());
   }
}

void GFXBeginModelPipelineState(ModelPipelineState state, int32_t texID, float testVal)
{
   glUseProgram(smState.modelProgram.programID);
   smState.currentProgram = smState.modelProgram.programID;
   GFXSetLightPos(smState.lightPos, smState.lightColor);
   GFXSetModelViewProjection(smState.modelMatrix, smState.viewMatrix, smState.projectionMatrix);
   
   glUniform1i(smState.modelProgram.uniformLocations[kUniform_SamplerID], 0);
   glUniform1f(smState.modelProgram.uniformLocations[kUniform_AlphaTestVal], testVal);
   glBindTexture(GL_TEXTURE_2D, smState.textures[texID]);
   
   switch (state)
   {
      case ModelPipeline_DefaultDiffuse:
         glDisable(GL_BLEND);
         glBlendFunc(GL_ONE, GL_ZERO);
         break;
      case ModelPipeline_AdditiveBlend:
         glEnable(GL_BLEND);
         glBlendEquation(GL_FUNC_ADD);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE);
         glUniform1f(smState.modelProgram.uniformLocations[kUniform_AlphaTestVal], 1.1f);
         break;
      case ModelPipeline_SubtractiveBlend:
         glEnable(GL_BLEND);
         glBlendEquation(GL_FUNC_ADD);
         glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
         glUniform1f(smState.modelProgram.uniformLocations[kUniform_AlphaTestVal], 1.1f);
         break;
      case ModelPipeline_TranslucentBlend:
         glEnable(GL_BLEND);
         glBlendEquation(GL_FUNC_ADD);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
         glUniform1f(smState.modelProgram.uniformLocations[kUniform_AlphaTestVal], 1.1f);
         break;
   }
   
   glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
   glEnableVertexAttribArray(kVertexAttrib_Position);
   glEnableVertexAttribArray(kVertexAttrib_Position_Normals);
   glEnableVertexAttribArray(kVertexAttrib_Position_TexCoords);
   glDisableVertexAttribArray(kVertexAttrib_Position_Color);
   glDisableVertexAttribArray(kVertexAttrib_Position_Next);
   glEnable(GL_CULL_FACE);
   glFrontFace(GL_CW);
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_LESS);
}

void GFXSetModelVerts(uint32_t modelId, uint32_t vertOffset, uint32_t texOffset)
{
   const uint32_t vertStride = sizeof(slm::vec3) + sizeof(slm::vec3);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, smState.indBuffers[modelId]);
   glBindBuffer(GL_ARRAY_BUFFER, smState.vertexBuffers[modelId]);
   glVertexAttribPointer(kVertexAttrib_Position, 3, GL_FLOAT, GL_FALSE, vertStride, ((uint8_t*)NULL)+(vertOffset*vertStride));
   glVertexAttribPointer(kVertexAttrib_Position_Normals, 3, GL_FLOAT, GL_FALSE, vertStride, ((uint8_t*)NULL)+sizeof(slm::vec3)+(vertOffset*vertStride));
   glBindBuffer(GL_ARRAY_BUFFER, smState.texVertBuffers[modelId]);
   glVertexAttribPointer(kVertexAttrib_Position_TexCoords, 2, GL_FLOAT, GL_FALSE, sizeof(slm::vec2), ((uint8_t*)NULL)+(texOffset*sizeof(slm::vec2)));
}

void GFXDrawModelPrims(uint32_t numVerts, uint32_t numInds, uint32_t startInds, uint32_t startVerts)
{
    glDrawRangeElementsBaseVertex(GL_TRIANGLES, 0, numVerts, numInds, GL_UNSIGNED_SHORT, ((uint16_t*)NULL) + startInds, startVerts);
}

void GFXBeginLinePipelineState()
{
   glUseProgram(smState.lineProgram.programID);
   smState.currentProgram = smState.lineProgram.programID;
   GFXSetModelViewProjection(smState.modelMatrix, smState.viewMatrix, smState.projectionMatrix);
   
   glDisable(GL_DEPTH_TEST);
   glDisable(GL_CULL_FACE);
   glDisable(GL_BLEND);
   
   glUniform2f(smState.lineProgram.uniformLocations[kLineUniform_ViewportScale], 1.0/smState.viewportSize.x, 1.0/smState.viewportSize.y);
   
   glEnableVertexAttribArray(kVertexAttrib_Position);
   glEnableVertexAttribArray(kVertexAttrib_Position_Normals);
   glDisableVertexAttribArray(kVertexAttrib_Position_TexCoords);
   glEnableVertexAttribArray(kVertexAttrib_Position_Color);
   glEnableVertexAttribArray(kVertexAttrib_Position_Next);
   
   glBindBuffer(GL_ARRAY_BUFFER, smState.lineVertexBuffer);
   
   // vc
   glVertexAttribPointer(kVertexAttrib_Position, 3, GL_FLOAT, GL_FALSE, kLineVertSize, NULL);
   // nvc
   glVertexAttribPointer(kVertexAttrib_Position_Next, 3, GL_FLOAT, GL_FALSE, kLineVertSize, ((uint8_t*)NULL)+sizeof(slm::vec3));
   // n
   glVertexAttribPointer(kVertexAttrib_Position_Normals, 3, GL_FLOAT, GL_FALSE, kLineVertSize, ((uint8_t*)NULL)+sizeof(slm::vec3)+sizeof(slm::vec3));
   // col
   glVertexAttribPointer(kVertexAttrib_Position_Color, 4, GL_FLOAT, GL_FALSE, kLineVertSize, ((uint8_t*)NULL)+sizeof(slm::vec3)+sizeof(slm::vec3)+sizeof(slm::vec3));
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
   
   glUniform1f(smState.lineProgram.uniformLocations[kLineUniform_Width], width);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
   glDrawArrays(GL_TRIANGLES, 0, 6);
}

#endif
