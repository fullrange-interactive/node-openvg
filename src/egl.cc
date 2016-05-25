#include "EGL/egl.h"
#include "EGL/eglext.h"
#include "GLES/gl.h"
#include "GLES/glext.h"
#include "VG/openvg.h"

#include "egl.h"
#include "bcm_host.h"

#include "argchecks.h"

#include "mire.c"

using namespace v8;
using namespace node;

egl::state_t egl::State;
EGLConfig egl::Config;

VGImage vg_image;
EGLImageKHR egl_image;
GLuint bufferText,mire;

float egl::p[4][2] = {
  {-1,-1},
  {1,-1},
  {1,1},
  {-1,1}
};

int egl::mire = -1;

extern void egl::InitBindings(Handle<Object> target) {
  NODE_SET_METHOD(target, "getError"      , egl::GetError);
  NODE_SET_METHOD(target, "swapBuffers"   , egl::SwapBuffers);
  NODE_SET_METHOD(target, "createPbufferFromClientBuffer",
                          egl::CreatePbufferFromClientBuffer);
  NODE_SET_METHOD(target, "destroySurface", egl::DestroySurface);

  NODE_SET_METHOD(target, "createContext" , egl::CreateContext);
  NODE_SET_METHOD(target, "destroyContext", egl::DestroyContext);
  NODE_SET_METHOD(target, "makeCurrent"   , egl::MakeCurrent);
}

extern void egl::Init()
{
  EGLBoolean result;
  int32_t success = 0;

  static const EGLint attribute_list_vg[] = {
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_ALPHA_MASK_SIZE, 8,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT & EGL_SWAP_BEHAVIOR_PRESERVED_BIT,    
    EGL_NONE
  };

  static const EGLint attribute_list_es[] = {
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_ALPHA_MASK_SIZE, 8,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT | EGL_OPENVG_BIT,    
    EGL_NONE
  };
  
  EGLint num_config;

  VC_RECT_T dst_rect;
  VC_RECT_T src_rect;
  DISPMANX_ELEMENT_HANDLE_T dispman_element;
  DISPMANX_DISPLAY_HANDLE_T dispman_display;
  DISPMANX_UPDATE_HANDLE_T  dispman_update;

  static EGL_DISPMANX_WINDOW_T nativewindow;

  // bcm_host_init() must be called before anything else
  // Equivalent: initNativeDisplay
  bcm_host_init();

  // get an EGL display connection
  // Equivalant: initNativeWindow
  State.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

  // create an EGL window surface
  success = graphics_get_display_size(0 /* LCD */ , &State.screen_width,
                                      &State.screen_height);
  assert(success >= 0);

  dst_rect.x = 0;
  dst_rect.y = 0;
  dst_rect.width  = State.screen_width;
  dst_rect.height = State.screen_height;

  src_rect.x = 0;
  src_rect.y = 0;
  src_rect.width  = State.screen_width  << 16;
  src_rect.height = State.screen_height << 16;

  dispman_display = vc_dispmanx_display_open(0 /* LCD */ );
  dispman_update  = vc_dispmanx_update_start(0);

  dispman_element =
    vc_dispmanx_element_add(dispman_update, 
                            dispman_display, 
                            0 /*layer */ ,
                            &dst_rect, 0 /*src */ , &src_rect,
                            DISPMANX_PROTECTION_NONE,
                            0 /*alpha */ , 0 /*clamp */ ,
                            DISPMANX_NO_ROTATE /*transform */);

  nativewindow.element = dispman_element;
  nativewindow.width   = State.screen_width;
  nativewindow.height  = State.screen_height;
  vc_dispmanx_update_submit_sync(dispman_update);

  int i32NumConfigs, i32MajorVersion, i32MinorVersion;
 
  /* Init to nothing */
  result = eglInitialize(State.display, NULL, NULL);
  
  assert(EGL_FALSE != result);

  /* Init to default display */
  result = eglInitialize(
    State.display,
    &i32MajorVersion,
    &i32MinorVersion);
  
  assert(EGL_FALSE != result);

  /**************  OPENGL ES init and configuration ************/
  
  /* Bind opengles API first */
  eglBindAPI(EGL_OPENGL_ES_API); 
  
  /* Get an appropriate EGL frame buffer configuration */
  result = eglChooseConfig(
    State.display, 
    attribute_list_es,
    &egl::Config,
    1,
    &num_config);
  
  assert(EGL_FALSE != result); 
  
  /* Get a surface */
  State.surface_ES = eglCreateWindowSurface(
    State.display,
    egl::Config,
    &nativewindow,
    NULL);
  
  assert(State.surface_ES != EGL_NO_SURFACE);  
  
  /* Create a context for opengl ES first */
  State.context_ES = eglCreateContext(
      State.display,
      egl::Config,
      EGL_NO_CONTEXT,
      NULL);
    
  assert(State.context_ES != EGL_NO_CONTEXT);   
  
  /************** OpenGL ES config and init ********************/

  /* Now bind OpenVG API */
  eglBindAPI(EGL_OPENVG_API); 
  
  /* Create a context for open VG now */
  State.context_VG = eglCreateContext(
      State.display,
      egl::Config,
      EGL_NO_CONTEXT,
      NULL);
  
  EGLint pbuffer_attrib[] = {
    EGL_WIDTH, State.screen_width,
    EGL_HEIGHT, State.screen_height,
    EGL_NONE
  };
  
  /* Create EGL pbuffer surface as surface. OpenVG will be draw in it. */
  State.surface_VG = eglCreatePbufferSurface(
    State.display,
    egl::Config,
    pbuffer_attrib);
  
  assert(State.surface_VG != EGL_NO_SURFACE);  
  
  /* Preserve color buffer when swapping openVG surface */
  eglSurfaceAttrib(
    State.display,
    State.surface_VG,
    EGL_SWAP_BEHAVIOR,
    EGL_BUFFER_PRESERVED);    
  
  /**************  Back to openGL ES **************************/
  
  eglBindAPI(EGL_OPENGL_ES_API);   
  
  /* Connect the ES context to the ES surface */
  result =   eglMakeCurrent(
    State.display,
    State.surface_ES,
    State.surface_ES,
    State.context_ES);
  
  assert(EGL_FALSE != result);

  /**************  Back to openVG *****************************/

  eglBindAPI(EGL_OPENVG_API); 
  
  /* Connect the VG context to the VG surface */
  result =   eglMakeCurrent(
    State.display,
    State.surface_VG,
    State.surface_VG,
    State.context_VG);

  /* Image to hold the whole canvas */
   vg_image = vgCreateImage(
     VG_sRGBA_8888,
     State.screen_width,
     State.screen_height,
     VG_IMAGE_QUALITY_FASTER);
   
   /* ...and it's underlying EGL buffer */
   egl_image = (EGLImageKHR)eglCreateImageKHR(
     State.display,
     State.context_VG,
     EGL_VG_PARENT_IMAGE_KHR,
     (EGLClientBuffer)(intptr_t)vg_image, NULL);

  /**************  Back to openGL ES **************************/
  
  eglBindAPI(EGL_OPENGL_ES_API);   
  
  /* Connect the ES context to the ES surface */
  result =   eglMakeCurrent(
    State.display,
    State.surface_ES,
    State.surface_ES,
    State.context_ES);
  
  assert(EGL_FALSE != result);
  
  /* Now, same as fireworks, generate texture and stuff */

  glBindTexture (GL_TEXTURE_2D,mire);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, State.screen_width, State.screen_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel_data);        
  
  glGenTextures (1,&bufferText);
  glBindTexture(GL_TEXTURE_2D, bufferText);

  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,GL_LINEAR);

  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, egl_image);
  
  /* Set initial OpenGL state */
  glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
  
  glDisable(GL_LIGHTING);
  glDisable (GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  
  glEnable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  
  glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
  
  glMatrixMode(GL_MODELVIEW);     
  
  
  /* Init left current context to openvg */
  

  /**************  Back to openVG *****************************/

  eglBindAPI(EGL_OPENVG_API); 
  
  /* Connect the VG context to the VG surface */
  result =   eglMakeCurrent(
    State.display,
    State.surface_VG,
    State.surface_VG,
    State.context_VG);  
}

extern void egl::Finish()
{
  glClear(GL_COLOR_BUFFER_BIT);
  
  eglSwapBuffers(State.display, State.surface_ES);
  eglMakeCurrent(State.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglSwapBuffers(State.display, State.surface_VG);
  eglMakeCurrent(State.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  
  eglDestroySurface(State.display, State.surface_ES);
  eglDestroyContext(State.display, State.context_ES);
  eglDestroySurface(State.display, State.surface_VG);
  eglDestroyContext(State.display, State.context_VG); 
  
  eglTerminate(State.display);

#ifdef __VIDEOCORE__
  bcm_host_deinit();
#endif
}

V8_METHOD(egl::GetError) {
  HandleScope scope;

  CheckArgs0(getError);

  V8_RETURN(scope.Close(Integer::New(eglGetError())));
}

V8_METHOD(egl::SwapBuffers)
{
  HandleScope scope;

  CheckArgs1(swapBuffers, surface, External);
  
  /* Before swapping ES surface, we must draw the texture */
  
  vgGetPixels(vg_image, 0, 0, 0,0, State.screen_width, State.screen_height);  
  
  /**************  Back to openGL ES **************************/
  
  eglBindAPI(EGL_OPENGL_ES_API);   
  
  /* Connect the ES context to the ES surface */
  eglMakeCurrent(
    State.display,
    State.surface_ES,
    State.surface_ES,
    State.context_ES);
  
  /* Just like fireworks */
  
  GLfloat orthoMat[16] =
  {
    1.0f/(State.screen_width/2.0f),   0,              0, 0,
    0,          1.0f/(State.screen_height/2.0f),       0, 0,
    0,          0,              1, 0,
    0,          0,              0, 1
  };
  

  GLubyte orthoIndices[] = {0,1,2,2,3,0};  
  
  GLfloat orthoVtx2[] = 
  {
    (State.screen_width/2.0f)*p[0][0], (State.screen_height/2.0f)*p[0][1], 0.0f,
    (State.screen_width/2.0f)*p[1][0], (State.screen_height/2.0f)*p[1][1], 0.0f,
    (State.screen_width/2.0f)*p[2][0], (State.screen_height/2.0f)*p[2][1], 0.0f,
    (State.screen_width/2.0f)*p[3][0], (State.screen_height/2.0f)*p[3][1], 0.0f
  };
  
  GLfloat orthoVtx[] = 
  {
    (State.screen_width/2.0f)*p[0][0], (State.screen_height/2.0f)*p[0][1], 0.0f,
    (State.screen_width/2.0f)*p[1][0], (State.screen_height/2.0f)*p[1][1], 0.0f,
    (State.screen_width/2.0f)*p[2][0], (State.screen_height/2.0f)*p[2][1], 0.0f,
    (State.screen_width/2.0f)*p[3][0], (State.screen_height/2.0f)*p[3][1], 0.0f
  };    
  
  float ax = p[2][0] - p[0][0];
  float ay = p[2][1] - p[0][1];
  float bx = p[3][0] - p[1][0];
  float by = p[3][1] - p[1][1];

  float cross = ax * by - ay * bx;

  float cy = p[0][1] - p[1][1];
  float cx = p[0][0] - p[1][0];

  float s = (ax * cy - ay * cx) / cross;

  float t = (bx * cy - by * cx) / cross;

  float q0 = 1 / (1 - t);
  float q1 = 1 / (1 - s);
  float q2 = 1 / t;
  float q3 = 1 / s;  
  
  GLfloat orthoTex[] =
  {
    0.0f*q0, 1.0f*q0, 0.0f*q0,1.0f*q0,
    1.0f*q1, 1.0f*q1, 0.0f*q1,1.0f*q1,    
    1.0f*q2, 0.0f*q2, 0.0f*q2,1.0f*q2,
    0.0f*q3, 0.0f*q3, 0.0f*q3,1.0f*q3
  };  
  
  int o[2] = {0,0};
  
  GLfloat orthoTex2[] =
  {
    0.0f*q0, 0.0f*q0, 0.0f*q0,1.0f*q0,
    1.0f*q1, 0.0f*q1, 0.0f*q1,1.0f*q1,    
    1.0f*q2, 1.0f*q2, 0.0f*q2,1.0f*q2,
    0.0f*q3, 1.0f*q3, 0.0f*q3,1.0f*q3
  };
  
  /* And draw on screen this time */
  glViewport(0, 0, State.screen_width, State.screen_height);

  glPushMatrix();

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();

  glLoadMatrixf(orthoMat);

  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glDisable(GL_BLEND);

  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);

  glBindTexture(GL_TEXTURE_2D, bufferText);

  glVertexPointer(3, GL_FLOAT, 0, orthoVtx2);
  glTexCoordPointer(4, GL_FLOAT, 0, orthoTex2);

  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, orthoIndices);

  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);

  glBindTexture(GL_TEXTURE_2D, 0);
  
  if(mire > 0)
  {
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    glVertexPointer(3, GL_FLOAT, 0, orthoVtx);
    glTexCoordPointer(4, GL_FLOAT, 0, orthoTex);
    
    glBindTexture(GL_TEXTURE_2D, mire);  

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, orthoIndices);
    
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);  
  }

  glPopMatrix();
  
  EGLSurface surface = (EGLSurface) External::Cast(*args[0])->Value();

  EGLBoolean result = eglSwapBuffers(State.display, State.surface_ES);

  /**************  Back to openVG *****************************/

  eglBindAPI(EGL_OPENVG_API); 
  
  /* Connect the VG context to the VG surface */
  result =   eglMakeCurrent(
    State.display,
    State.surface_VG,
    State.surface_VG,
    State.context_VG);  
  
  V8_RETURN(scope.Close(Boolean::New(result)));
}

V8_METHOD(egl::CreatePbufferFromClientBuffer) {
  HandleScope scope;

  // According to the spec (sec. 4.2.2 EGL Functions)
  // The buffer is a VGImage: "The VGImage to be targeted is cast to the
  // EGLClientBuffer type and passed as the buffer parameter."
  // So, check for a Number (as VGImages are checked on openvg.cc) and
  // cast to a EGLClientBuffer.

  CheckArgs1(CreatePbufferFromClientBuffer, vgImage, Number);

  EGLClientBuffer buffer =
    reinterpret_cast<EGLClientBuffer>(args[0]->Uint32Value());

  static const EGLint attribute_list[] = {
    EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
    EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
    EGL_MIPMAP_TEXTURE, EGL_FALSE,
    EGL_NONE
  };

  EGLSurface surface =
    eglCreatePbufferFromClientBuffer(State.display,
                                     EGL_OPENVG_IMAGE,
                                     buffer,
                                     egl::Config,
                                     attribute_list);

  V8_RETURN(scope.Close(External::New(surface)));
}

V8_METHOD(egl::DestroySurface) {
  HandleScope scope;

  CheckArgs1(destroySurface, surface, External);

  EGLSurface surface = (EGLSurface) External::Cast(*args[0])->Value();

  EGLBoolean result = eglDestroySurface(State.display, surface);

  V8_RETURN(scope.Close(Boolean::New(result)));
}

V8_METHOD(egl::MakeCurrent) {
  HandleScope scope;

  CheckArgs2(makeCurrent, surface, External, context, External);

  EGLSurface surface = (EGLSurface) External::Cast(*args[0])->Value();
  EGLContext context = (EGLContext) External::Cast(*args[1])->Value();

  // According to EGL 1.4 spec, 3.7.3, for OpenVG contexts, draw and read
  // surfaces must be the same
  EGLBoolean result = eglMakeCurrent(State.display, surface, surface, context);

  V8_RETURN(scope.Close(Boolean::New(result)));
}

V8_METHOD(egl::CreateContext) {
  HandleScope scope;

  // No arg checks

  EGLContext shareContext = args.Length() == 0 ?
    EGL_NO_CONTEXT :
    (EGLContext) External::Cast(*args[0])->Value();

  // According to EGL 1.4 spec, 3.7.3, for OpenVG contexts, draw and read
  // surfaces must be the same
  EGLContext result =
    eglCreateContext(State.display, egl::Config, shareContext, NULL);

  V8_RETURN(scope.Close(External::New(result)));
}

V8_METHOD(egl::DestroyContext) {
  HandleScope scope;

  CheckArgs1(destroyContext, context, External);

  EGLContext context = (EGLContext) External::Cast(*args[0])->Value();

  EGLBoolean result = eglDestroyContext(State.display, context);

  V8_RETURN(scope.Close(Boolean::New(result)));
}
