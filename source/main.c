#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/player.h"

#include <psp2/kernel/threadmgr.h>
#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <sys/types.h>
#include <malloc.h>
#include <pthread.h>
#include <psp2/touch.h>
#include <math.h>
#include <stdbool.h>
#include <psp2/ctrl.h>
#include <string.h>
#include <pthread-svelte/include/pthread_svelte.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/sysmodule.h>

extern so_module so_mod;
extern so_module so_mod_aux;
extern JNIEnv jni;   // Definida en init.c

bool video_player_active = false;
bool video_player_wantexit = false;

int _newlib_heap_size_user = 280 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

// Prototipos con la firma correcta (void*)
void* game_thread(void *arg);
void* controls_thread(void *arg);

// Punteros a funciones del juego (MC2)
void (* GL2JNILib_touchEvent)(JNIEnv *env, jclass clazz, jint p1, jint p2, jint p3, jint p4);
void (* GL2JNILib_keyboardEvent)(JNIEnv *env, jclass clazz, jint key, jint isDown);
void (* GL2JNILib_setTouchPadDTLeft)(JNIEnv *env, jclass clazz, jfloat x, jfloat y, jint pointer_id);
void (* GL2JNILib_setTouchPadDT)(JNIEnv *env, jclass clazz, jfloat x, jfloat y, jint pointer_id);
int (* GL2JNILib_isGamePlay)();

void (* Game_nativeInit)(JNIEnv *env, jclass clazz);
void (* GameRenderer_nativeInit)(JNIEnv *env, jclass clazz);
void (* GameRenderer_nativeResize)(JNIEnv *env, jclass clazz, jint w, jint h);
void (* GameRenderer_nativeRender)(JNIEnv *env, jclass clazz);
void (* Game_nativeOnKeyDown)(JNIEnv *env, jclass clazz, jint keyCode);
void (* Game_nativeOnKeyUp)(JNIEnv *env, jclass clazz, jint keyCode);
void (* Game_nativeAccelerator)(JNIEnv *env, jclass clazz, jfloat x, jfloat y, jfloat z);
void (* Game_nativeOrientation)(JNIEnv *env, jclass clazz, jint orientation);
int (* Game_nativeGetInfo)(JNIEnv *env, jclass clazz);
void (* GameGLSurfaceView_nativeOnTouch)(JNIEnv *env, jclass clazz, jint action, jint x, jint y, jint pointerId);
void (* GameGLSurfaceView_nativePause)(JNIEnv *env, jclass clazz);
void (* GameGLSurfaceView_nativeResume)(JNIEnv *env, jclass clazz);
void (* GLMediaPlayer_nativeInit)(JNIEnv *env, jclass clazz);

// ---------- Funciones auxiliares (pollTouch, pollPad, etc.) ----------
int lastX[SCE_TOUCH_MAX_REPORT] = {-1, -1, -1, -1, -1, -1, -1, -1};
int lastY[SCE_TOUCH_MAX_REPORT] = {-1, -1, -1, -1, -1, -1, -1, -1};

float lerp(float x1, float y1, float x3, float y3, float x2) {
    return ((x2-x1)*(y3-y1) / (x3-x1)) + y1;
}

void coord_normalize(float * x, float * y, float deadzone) {
    float magnitude = sqrtf((*x * *x) + (*y * *y));
    if (magnitude < deadzone) {
        *x = 0;
        *y = 0;
        return;
    }
    *x = *x / magnitude;
    *y = *y / magnitude;
    float multiplier = ((magnitude - deadzone) / (1 - deadzone));
    *x = *x * multiplier;
    *y = *y * multiplier;
}

SceTouchData touch_old;
void pollTouch() {
    SceTouchData touch;
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    for (int i = 0; i < touch.reportNum; i++) {
        int x = (int) ((float) touch.report[i].x * 960.f / 1920.0f);
        int y = (int) ((float) touch.report[i].y * 544.f / 1088.0f);

        int finger_down = 0;
        if (touch_old.reportNum > 0) {
            for (int j = 0; j < touch_old.reportNum; j++) {
                if (touch.report[i].id == touch_old.report[j].id) {
                    finger_down = 1;
                }
            }
        }

        if (!finger_down) {
            GL2JNILib_touchEvent(&jni, (void *) 0x42424242, 1, x, y, touch.report[i].id);
        } else {
            GL2JNILib_touchEvent(&jni, (void *) 0x42424242, 2, x, y, touch.report[i].id);
        }
    }

    for (int i = 0; i < touch_old.reportNum; i++) {
        int finger_up = 1;
        if (touch.reportNum > 0) {
            for (int j = 0; j < touch.reportNum; j++) {
                if (touch.report[j].id == touch_old.report[i].id) {
                    finger_up = 0;
                }
            }
        }
        if (finger_up == 1) {
            int x = (int) ((float) touch_old.report[i].x * 960.f / 1920.0f);
            int y = (int) ((float) touch_old.report[i].y * 544.f / 1088.0f);
            GL2JNILib_touchEvent(&jni, (void*)0x42424242, 0, x, y, touch_old.report[i].id);
        }
    }
    sceClibMemcpy(&touch_old, &touch, sizeof(touch));
}

enum {
    AKEYCODE_BACK = 4,
    AKEYCODE_DPAD_UP = 19,
    AKEYCODE_DPAD_DOWN = 20,
    AKEYCODE_DPAD_LEFT = 21,
    AKEYCODE_DPAD_RIGHT = 22,
    AKEYCODE_DPAD_CENTER = 23,
    AKEYCODE_A = 29,
    AKEYCODE_B = 30,
    AKEYCODE_BUTTON_X = 99,
    AKEYCODE_BUTTON_Y = 100,
    AKEYCODE_BUTTON_L1 = 102,
    AKEYCODE_BUTTON_R1 = 103,
    AKEYCODE_BUTTON_START = 108,
    AKEYCODE_BUTTON_SELECT = 109,
    AKEYCODE_MOVE_END = 123
};

typedef struct {
    uint32_t sce_button;
    uint32_t android_button;
} ButtonMapping;

#define L_INNER_DEADZONE 0.2f
#define R_INNER_DEADZONE 0.2f

static ButtonMapping mapping[] = {
    { SCE_CTRL_UP,        AKEYCODE_DPAD_UP },
    { SCE_CTRL_DOWN,      AKEYCODE_DPAD_DOWN },
    { SCE_CTRL_LEFT,      AKEYCODE_DPAD_LEFT },
    { SCE_CTRL_RIGHT,     AKEYCODE_DPAD_RIGHT },
    { SCE_CTRL_CROSS,     AKEYCODE_DPAD_CENTER },
    { SCE_CTRL_CIRCLE,    AKEYCODE_MOVE_END },
    { SCE_CTRL_SQUARE,    AKEYCODE_BUTTON_X },
    { SCE_CTRL_TRIANGLE,  AKEYCODE_BUTTON_Y },
    { SCE_CTRL_L1,        AKEYCODE_BUTTON_L1 },
    { SCE_CTRL_R1,        AKEYCODE_BUTTON_R1 },
    { SCE_CTRL_START,     AKEYCODE_BUTTON_START },
    { SCE_CTRL_SELECT,    AKEYCODE_BUTTON_SELECT },
};

uint32_t old_buttons = 0, current_buttons = 0, pressed_buttons = 0, released_buttons = 0;
float lastLx = 0.0f, lastLy = 0.0f, lastRx = 0.0f, lastRy = 0.0f;
float lastLastLx = 0.0f, lastLastLy = 0.0f, lastLastRx = 0.0f, lastLastRy = 0.0f;
float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;

void pollPad() {
    SceCtrlData pad;
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);

    { // Gamepad buttons
        old_buttons = current_buttons;
        current_buttons = pad.buttons;
        pressed_buttons = current_buttons & ~old_buttons;
        released_buttons = ~current_buttons & old_buttons;

        for (int i = 0; i < sizeof(mapping) / sizeof(ButtonMapping); i++) {
            if (pressed_buttons & mapping[i].sce_button) {
                GL2JNILib_keyboardEvent(&jni, (void *) 0x42424242, mapping[i].android_button, 1);
            }
            if (released_buttons & mapping[i].sce_button) {
                GL2JNILib_keyboardEvent(&jni, (void *) 0x42424242, mapping[i].android_button, 0);
            }
        }
    }

    // Sticks
    lx = ((float)pad.lx - 128.0f) / 128.0f;
    ly = ((float)pad.ly - 128.0f) / 128.0f;
    rx = ((float)pad.rx - 128.0f) / 128.0f;
    ry = ((float)pad.ry - 128.0f) / 128.0f;
    coord_normalize(&lx, &ly, L_INNER_DEADZONE);
    coord_normalize(&rx, &ry, R_INNER_DEADZONE);

    if ((lx == 0.f && ly == 0.f) && (lastLx == 0.f && lastLy == 0.f) && (lastLastLx != 0.f || lastLastLy != 0.f)) {
        GL2JNILib_setTouchPadDTLeft(&jni, (void *) 0x42424242, lx, ly, 1);
    }
    if ((rx == 0.f && ry == 0.f) && (lastRx == 0.f && lastRy == 0.f) && (lastLastRx != 0.f || lastLastRy != 0.f)) {
        GL2JNILib_setTouchPadDT(&jni, (void *) 0x42424242, rx, ry, 2);
    }
    if ((lx != 0.f || ly != 0.f) || ((lx == 0.f && ly == 0.f) && (lastLx != 0.f || lastLy != 0.f))) {
        GL2JNILib_setTouchPadDTLeft(&jni, (void *) 0x42424242, lx * 1.10f, ly * 1.10f * -1, 1);
    }
    if ((rx != 0.f || ry != 0.f) || ((rx == 0.f && ry == 0.f) && (lastRx != 0.f || lastRy != 0.f))) {
        GL2JNILib_setTouchPadDT(&jni, (void *) 0x42424242, rx, ry, 2);
    }

    lastLastLx = lastLx;
    lastLastLy = lastLy;
    lastLastRx = lastRx;
    lastLastRy = lastRy;
    lastLx = lx;
    lastLy = ly;
    lastRx = rx;
    lastRy = ry;
}

// ---------- Hilo de controles ----------
void * controls_thread(void *arg) {
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x80000);
    while (1) {
        pollTouch();
        pollPad();
        sceKernelDelayThread(8335);
    }
}

// ---------- Inicialización de controles ----------
void controls_init() {
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 1);

    GL2JNILib_touchEvent = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameGLSurfaceView_nativeOnTouch");
    GL2JNILib_keyboardEvent = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeOnKeyDown");
    GL2JNILib_setTouchPadDTLeft = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeSetTouchPadDTLeft");
    GL2JNILib_setTouchPadDT = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeSetTouchPadDT");
    GL2JNILib_isGamePlay = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeIsGamePlay");
}

// ---------- Función principal del juego (hilo) ----------
void* game_thread(void *arg) {
    controls_init();

    void (* GL2JNILib_init)(JNIEnv * env, jclass clazz) = (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameGLSurfaceView_nativeInit");
    if (GL2JNILib_init) GL2JNILib_init(&jni, (jclass)0x42424242);
    l_info("GameGLSurfaceView_nativeInit passed");

    GameRenderer_nativeInit = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameRenderer_nativeInit");
    if (GameRenderer_nativeInit) GameRenderer_nativeInit(&jni, (jclass)0x42424242);
    l_info("GameRenderer_nativeInit passed");

    Game_nativeInit = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeInit");
    if (Game_nativeInit) Game_nativeInit(&jni, (jclass)0x42424242);
    l_info("Game_nativeInit passed");

    GameRenderer_nativeResize = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameRenderer_nativeResize");
    if (GameRenderer_nativeResize) GameRenderer_nativeResize(&jni, (jclass)0x42424242, 960, 544);
    l_info("GameRenderer_nativeResize passed");

    void (* GLMediaPlayer_nativeInit)(JNIEnv * env, jclass clazz) = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GLMediaPlayer_nativeInit");
    if (GLMediaPlayer_nativeInit) GLMediaPlayer_nativeInit(&jni, (jclass)0x42424242);
    l_info("GLMediaPlayer_nativeInit passed");

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_create(&t, &attr, controls_thread, NULL);
    pthread_detach(t);

    sceKernelDelayThread(200000);
    l_info("Controls thread initialized");

    int (* GameRenderer_nativeRender)(JNIEnv * env, jclass clazz) = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameRenderer_nativeRender");
    if (!GameRenderer_nativeRender) {
        l_fatal("Could not find nativeRender function");
        return NULL;
    }

    const uint32_t delta = (1000000 / (24+1));
    uint32_t last_render_time = sceKernelGetProcessTimeLow();

    while (1) {
        if (video_player_active) {
            glViewport(0, 0, 960, 544);
            glScissor(0, 0, 960, 544);
            glClear(GL_COLOR_BUFFER_BIT);
            draw_2d_frame();
            gl_swap();
            if (video_player_wantexit) {
                video_close();
                video_player_wantexit = false;
            }
        } else {
            GameRenderer_nativeRender(&jni, (jclass)0x42424242);
            gl_swap();
            while (sceKernelGetProcessTimeLow() - last_render_time < delta) {
                sched_yield();
            }
            last_render_time = sceKernelGetProcessTimeLow();
        }
    }
    sceKernelExitDeleteThread(0);
}

// ---------- main ----------
int main() {
    soloader_init_all();
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);

    int (* JNI_OnLoad)(void *jvm) = (void *)so_symbol(&so_mod, "JNI_OnLoad");
    if (JNI_OnLoad) JNI_OnLoad(&jvm);

    // Crear hilo principal del juego con pila grande
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 9 * 1024 * 1024);
    pthread_create(&t, &attr, game_thread, NULL);
    pthread_join(t, NULL);
    return 0;
}
