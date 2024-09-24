#include <SDL3/SDL.h>

#if defined(__APPLE__)
#include <Foundation/Foundation.h>
#include <QuartzCore/CAMetalLayer.h>
#include <Cocoa/Cocoa.h>
#endif

extern "C"
{
#if defined(__APPLE__) || defined(WGPU_NATIVE)
#include "webgpu.h"
#else
#include <webgpu/webgpu.h>
#endif
}

#if defined(__APPLE__)
void GFXSetCocoaWindow(SDL_Window* window, WGPUSurfaceDescriptorFromMetalLayer* s)
{
   NSWindow *nsWindow = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);

    [nsWindow.contentView setWantsLayer : YES];
    id metalLayer = [CAMetalLayer layer];
    [nsWindow.contentView setLayer : metalLayer];

    s->chain.next = 0;
    s->chain.sType = WGPUSType_SurfaceDescriptorFromMetalLayer;
    s->layer = metalLayer;
}
#endif

