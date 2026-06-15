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

// Declarar módulos externos (definidos en init.c)
extern so_module so_mod;
extern so_module so_mod_aux;

bool video_player_active = false;
bool video_player_wantexit = false;

int _newlib_heap_size_user = 280 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

void* game_thread();
void * controls_thread();

// Prototipos de funciones del juego (MC2)
void (* GL2JNILib_touchEvent)(JNIEnv *env, jclass clazz, jint p1, jint p2, jint p3, jint p4);
void (* GL2JNILib_keyboardEvent)(JNIEnv *env, jclass clazz, jint key, jint isDown);
void (* GL2JNILib_setTouchPadDTLeft)(JNIEnv *env, jclass clazz, jfloat x, jfloat y, jint pointer_id);
void (* GL2JNILib_setTouchPadDT)(JNIEnv *env, jclass clazz, jfloat x, jfloat y, jint pointer_id);
int (* GL2JNILib_isGamePlay)();

// Funciones específicas de MC2 (extraídas con nm)
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
// ... añade más según necesites

int main() {
    soloader_init_all();

    sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);

    // Llamar JNI_OnLoad de la librería principal (si existe)
    int (* JNI_OnLoad)(void *jvm) = (void *)so_symbol(&so_mod, "JNI_OnLoad");
    if (JNI_OnLoad) JNI_OnLoad(&jvm);

    // DRM (esto puede ser diferente en MC2, lo dejamos por ahora)
    int **lockPointer1 = (uintptr_t)so_mod.text_base + 0x0098a278; // OJO: estas offsets son de MC3, para MC2 habrá que buscarlas luego
    int **lockPointer2 = (uintptr_t)so_mod.text_base + 0x0098a26c;
    int **lockPointer3 = (uintptr_t)so_mod.text_base + 0x0098a27c;

    *lockPointer1 = malloc(4);
    *lockPointer2 = malloc(4);
    *lockPointer3 = malloc(4);

    **lockPointer1 = 1;
    **lockPointer2 = 1;
    **lockPointer3 = 1;

    // Ejecutar el .so en un hilo con pila grande
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 9 * 1024 * 1024);
    pthread_create(&t, &attr, game_thread, NULL);
    pthread_join(t, NULL);
}

// ... (las funciones pollTouch, pollPad, controls_thread se mantienen igual, solo cambian los nombres de las funciones que usan)

void controls_init() {
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 1);

    // Cargar los símbolos desde la librería principal (libsandstorm2.so)
    GL2JNILib_touchEvent = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameGLSurfaceView_nativeOnTouch");
    GL2JNILib_keyboardEvent = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeOnKeyDown");  // ojo: hay dos funciones, onKeyDown y onKeyUp
    GL2JNILib_setTouchPadDTLeft = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeSetTouchPadDTLeft"); // puede no existir; si no, buscar alternativa
    GL2JNILib_setTouchPadDT = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeSetTouchPadDT");
    GL2JNILib_isGamePlay = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeIsGamePlay");  // probablemente no existe, luego lo ajustamos
}

// ... (pollTouch y pollPad se mantienen igual, solo que usan GL2JNILib_touchEvent, etc.)

void* game_thread() {
    controls_init();

    // Inicializaciones según los nombres de MC2
    void (* GL2JNILib_init)(JNIEnv * env, jclass clazz) = (void *)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameGLSurfaceView_nativeInit");
    if (GL2JNILib_init) GL2JNILib_init(&jni, (jclass)0x42424242);
    l_info("GameGLSurfaceView_nativeInit passed");

    // Inicialización del renderizador
    GameRenderer_nativeInit = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameRenderer_nativeInit");
    if (GameRenderer_nativeInit) GameRenderer_nativeInit(&jni, (jclass)0x42424242);
    l_info("GameRenderer_nativeInit passed");

    // Inicialización del juego principal
    Game_nativeInit = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_Game_nativeInit");
    if (Game_nativeInit) Game_nativeInit(&jni, (jclass)0x42424242);
    l_info("Game_nativeInit passed");

    // Redimensionar la superficie
    GameRenderer_nativeResize = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameRenderer_nativeResize");
    if (GameRenderer_nativeResize) GameRenderer_nativeResize(&jni, (jclass)0x42424242, 960, 544);
    l_info("GameRenderer_nativeResize passed");

    // Inicializar media player si es necesario
    void (* GLMediaPlayer_nativeInit)(JNIEnv * env, jclass clazz) = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GLMediaPlayer_nativeInit");
    if (GLMediaPlayer_nativeInit) GLMediaPlayer_nativeInit(&jni, (jclass)0x42424242);
    l_info("GLMediaPlayer_nativeInit passed");

    // Lanzar el hilo de controles
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_create(&t, &attr, controls_thread, NULL);
    pthread_detach(t);

    sceKernelDelayThread(200000);
    l_info("Controls thread initialized");

    // Función de paso (step) del juego
    int (* GameRenderer_nativeRender)(JNIEnv * env, jclass clazz) = (void*)so_symbol(&so_mod, "Java_com_gameloft_android_GAND_GloftBPHP_ML_GameRenderer_nativeRender");
    if (!GameRenderer_nativeRender) {
        l_fatal("Could not find nativeRender function");
        return NULL;
    }

    const uint32_t delta = (1000000 / (24+1));
    uint32_t last_render_time = sceKernelGetProcessTimeLow();

    while (1) {
        if (video_player_active) {
            l_info("about to draw 2d frame");
            glViewport(0, 0, 960, 544);
            glScissor(0, 0, 960, 544);
            glClear(GL_COLOR_BUFFER_BIT);
            draw_2d_frame();
            gl_swap();

            if (video_player_wantexit == true) {
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
            // is_gameplay = (GL2JNILib_isGamePlay ? GL2JNILib_isGamePlay() : 0);
        }
    }

    sceKernelExitDeleteThread(0);
}
