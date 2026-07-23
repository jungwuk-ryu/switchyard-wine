/*
 * Switchyard Mesa OpenGL backend smoke test.
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

static int fail( const char *message )
{
    fprintf( stderr, "%s\n", message );
    return 1;
}

int main( int argc, char **argv )
{
    static const char class_name[] = "SwitchyardMesaOpenGLBackendTest";
    WNDCLASSA class = {0};
    PIXELFORMATDESCRIPTOR format = {0};
    const char *renderer;
    const char *version;
    HGLRC context;
    HWND window;
    HDC dc;
    int pixel_format;
    int major = 0, minor = 0;
    BOOL require_llvmpipe = argc > 1 && !strcmp( argv[1], "--require-llvmpipe" );

    class.lpfnWndProc = DefWindowProcA;
    class.hInstance = GetModuleHandleW( NULL );
    class.lpszClassName = class_name;
    if (!RegisterClassA( &class )) return fail( "could not register the OpenGL test window" );

    window = CreateWindowA( class_name, class_name, WS_POPUP, 0, 0, 16, 16,
                            NULL, NULL, class.hInstance, NULL );
    if (!window) return fail( "could not create the OpenGL test window" );
    dc = GetDC( window );

    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 24;
    format.cDepthBits = 24;
    format.iLayerType = PFD_MAIN_PLANE;
    if (!(pixel_format = ChoosePixelFormat( dc, &format )))
        return fail( "could not choose an OpenGL pixel format" );
    if (!SetPixelFormat( dc, pixel_format, &format ))
        return fail( "could not set the OpenGL pixel format" );
    if (!(context = wglCreateContext( dc )))
        return fail( "could not create an OpenGL context" );
    if (!wglMakeCurrent( dc, context ))
        return fail( "could not make the OpenGL context current" );

    version = (const char *)glGetString( GL_VERSION );
    renderer = (const char *)glGetString( GL_RENDERER );
    if (!version || !renderer) return fail( "OpenGL did not report a version and renderer" );
    sscanf( version, "%d.%d", &major, &minor );
    printf( "version=%s\nrenderer=%s\n", version, renderer );

    if (require_llvmpipe)
    {
        if (major < 4 || (major == 4 && minor < 3))
            return fail( "Mesa backend did not provide OpenGL 4.3 or newer" );
        if (!strstr( renderer, "llvmpipe" ))
            return fail( "Mesa backend did not select llvmpipe" );
    }
    else if (strstr( renderer, "llvmpipe" ))
    {
        return fail( "Mesa backend was selected without the runtime option" );
    }

    wglMakeCurrent( NULL, NULL );
    wglDeleteContext( context );
    ReleaseDC( window, dc );
    DestroyWindow( window );
    UnregisterClassA( class_name, class.hInstance );
    return 0;
}
