#if defined(__mac__)

#include <OpenGL/gl.h>
#include <OpenGL/glu.h>

#elif defined(_MSC_BUILD)

// Visual Studio Build uses SDL openGL headers

#include <SDL/SDL.h>
#include <SDL/SDL_opengl.h>

#include <GL/gl.h>
#include <GL/glu.h>

#else

#include <GL/gl.h>
#include <GL/glu.h>

#if defined(WIN_32)
// on Windows, some stuff that's normally in gl.h (1.2 and 1.3 stuff) is
// in glext
#include <GL/glext.h>
#endif

#endif
