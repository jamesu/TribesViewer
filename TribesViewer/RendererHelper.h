// API

#include <stdint.h>
#include <slm/slmath.h>

class Bitmap;
class Palette;
class SDL_Window;
class SDL_Renderer;

extern int GFXSetup(SDL_Window* window, SDL_Renderer* renderer);
extern void GFXTeardown();
extern void GFXTestRender(slm::vec3 pos);
extern void GFXPollEvents();
extern void GFXResetSwapChain();

extern bool GFXBeginFrame();
extern void GFXEndFrame();
extern void GFXHandleResize();

extern int32_t GFXLoadTexture(Bitmap* bmp, Palette*pal);
extern void GFXDeleteTexture(int32_t texID);
extern void GFXLoadModelData(uint32_t modelId, void* verts, void* texverts, void* inds, uint32_t numVerts, uint32_t numTexVerts, uint32_t numInds);
extern void GFXSetModelViewProjection(slm::mat4 &model, slm::mat4 &view, slm::mat4 &proj);
extern void GFXSetLightPos(slm::vec3 pos, slm::vec4 ambient);
extern void GFXBeginModelPipelineState(ModelPipelineState state, int32_t texID, float testVal);
extern void GFXSetModelVerts(uint32_t modelId, uint32_t vertOffset, uint32_t texOffset);
extern void GFXDrawModelPrims(uint32_t numVerts, uint32_t numInds, uint32_t startInds, uint32_t startVerts);
extern void GFXBeginLinePipelineState();
extern void GFXDrawLine(slm::vec3 start, slm::vec3 end, slm::vec4 color, float width);

