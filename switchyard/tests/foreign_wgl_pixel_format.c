#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <GL/gl.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define WM_REPORT_PIXEL_FORMAT (WM_APP + 1)

static int reported_pixel_format;

static LRESULT CALLBACK window_proc( HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam )
{
    if (message == WM_REPORT_PIXEL_FORMAT)
    {
        reported_pixel_format = wparam;
        return 0;
    }
    return DefWindowProcW( hwnd, message, wparam, lparam );
}

static int child_main( HWND hwnd )
{
    PIXELFORMATDESCRIPTOR pfd = {0};
    HGLRC context = NULL;
    HDC dc = NULL;
    int format, i;
    int ret = 1;

    dc = GetDC( hwnd );
    if (!dc)
    {
        fprintf( stderr, "GetDC failed: %lu\n", GetLastError() );
        goto done;
    }

    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;

    if (!(format = ChoosePixelFormat( dc, &pfd )))
    {
        fprintf( stderr, "ChoosePixelFormat failed: %lu\n", GetLastError() );
        goto done;
    }
    if (!SetPixelFormat( dc, format, &pfd ))
    {
        fprintf( stderr, "SetPixelFormat failed: %lu\n", GetLastError() );
        goto done;
    }
    if (GetPixelFormat( dc ) != format)
    {
        fprintf( stderr, "foreign GetPixelFormat did not preserve format %d\n", format );
        goto done;
    }

    if (!(context = wglCreateContext( dc )))
    {
        fprintf( stderr, "wglCreateContext failed: %lu\n", GetLastError() );
        goto done;
    }
    if (!wglMakeCurrent( dc, context ))
    {
        fprintf( stderr, "wglMakeCurrent failed: %lu\n", GetLastError() );
        goto done;
    }
    glViewport( 0, 0, 128, 128 );

    for (i = 0; i < 3; ++i)
    {
        glClearColor( i == 0, i == 1, i == 2, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
        if (!SwapBuffers( dc ))
        {
            fprintf( stderr, "SwapBuffers failed: %lu\n", GetLastError() );
            goto done;
        }
        if (GetPixelFormat( dc ) != format)
        {
            fprintf( stderr, "foreign pixel format was lost after present %d\n", i );
            goto done;
        }
        if (!SetPixelFormat( dc, format, &pfd ))
        {
            fprintf( stderr, "setting the preserved pixel format failed: %lu\n", GetLastError() );
            goto done;
        }
    }

    if (!wglMakeCurrent( NULL, NULL ))
    {
        fprintf( stderr, "clearing the WGL context failed: %lu\n", GetLastError() );
        goto done;
    }
    if (!ReleaseDC( hwnd, dc ))
    {
        fprintf( stderr, "ReleaseDC failed: %lu\n", GetLastError() );
        dc = NULL;
        goto done;
    }
    dc = GetDC( hwnd );
    if (!dc || GetPixelFormat( dc ) != format)
    {
        fprintf( stderr, "foreign pixel format was lost across DC acquisition\n" );
        goto done;
    }

    PostMessageW( hwnd, WM_REPORT_PIXEL_FORMAT, format, 0 );
    printf( "Foreign WGL pixel format %d remained stable across presents.\n", format );
    ret = 0;

done:
    wglMakeCurrent( NULL, NULL );
    if (context) wglDeleteContext( context );
    if (dc) ReleaseDC( hwnd, dc );
    return ret;
}

static int parent_main( const WCHAR *executable )
{
    WNDCLASSW class = {0};
    PROCESS_INFORMATION process = {0};
    STARTUPINFOW startup = {0};
    WCHAR command[2048];
    DWORD exit_code = 1;
    HWND hwnd = NULL;
    HDC dc = NULL;
    MSG message;
    int ret = 1;

    class.lpfnWndProc = window_proc;
    class.hInstance = GetModuleHandleW( NULL );
    class.lpszClassName = L"SwitchyardForeignWglTest";
    if (!RegisterClassW( &class ))
    {
        fprintf( stderr, "RegisterClass failed: %lu\n", GetLastError() );
        goto done;
    }
    hwnd = CreateWindowW( class.lpszClassName, L"Switchyard foreign WGL test",
                          WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                          320, 240, NULL, NULL, class.hInstance, NULL );
    if (!hwnd)
    {
        fprintf( stderr, "CreateWindow failed: %lu\n", GetLastError() );
        goto done;
    }

    startup.cb = sizeof(startup);
    swprintf( command, sizeof(command) / sizeof(command[0]),
              L"\"%ls\" --child 0x%llx", executable,
              (unsigned long long)(ULONG_PTR)hwnd );
    if (!CreateProcessW( NULL, command, NULL, NULL, FALSE, 0, NULL, NULL,
                         &startup, &process ))
    {
        fprintf( stderr, "CreateProcess failed: %lu\n", GetLastError() );
        goto done;
    }

    for (;;)
    {
        DWORD wait = MsgWaitForMultipleObjects( 1, &process.hProcess, FALSE,
                                                30000, QS_ALLINPUT );

        while (PeekMessageW( &message, NULL, 0, 0, PM_REMOVE ))
        {
            TranslateMessage( &message );
            DispatchMessageW( &message );
        }
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1)
        {
            fprintf( stderr, "child process wait failed or timed out: %lu\n", wait );
            TerminateProcess( process.hProcess, 2 );
            goto done;
        }
    }
    while (PeekMessageW( &message, NULL, 0, 0, PM_REMOVE ))
    {
        TranslateMessage( &message );
        DispatchMessageW( &message );
    }

    if (!GetExitCodeProcess( process.hProcess, &exit_code ) || exit_code)
    {
        fprintf( stderr, "foreign WGL child failed with status %lu\n", exit_code );
        goto done;
    }
    if (!reported_pixel_format)
    {
        fprintf( stderr, "foreign WGL child did not report a pixel format\n" );
        goto done;
    }
    dc = GetDC( hwnd );
    if (!dc || GetPixelFormat( dc ) != reported_pixel_format)
    {
        fprintf( stderr, "owner did not receive foreign pixel format %d\n",
                 reported_pixel_format );
        goto done;
    }
    printf( "Foreign WGL owner received pixel format %d.\n", reported_pixel_format );
    ret = 0;

done:
    if (dc) ReleaseDC( hwnd, dc );
    if (process.hThread) CloseHandle( process.hThread );
    if (process.hProcess) CloseHandle( process.hProcess );
    if (hwnd) DestroyWindow( hwnd );
    if (class.lpszClassName) UnregisterClassW( class.lpszClassName, class.hInstance );
    return ret;
}

int wmain( int argc, WCHAR **argv )
{
    if (argc == 3 && !wcscmp( argv[1], L"--child" ))
        return child_main( (HWND)(ULONG_PTR)_wcstoui64( argv[2], NULL, 0 ) );
    return parent_main( argv[0] );
}
