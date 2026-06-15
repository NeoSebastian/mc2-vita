/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"
#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/settings.h"
#include "utils/trophies.h"
#include "utils/utils.h"
#include <string.h>
#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>
#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>
#include <stdatomic.h>
#include <kubridge.h>
#include <malloc.h>

// Definiciones manuales para tipos de memoria (si no están en kubridge)
#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX  0x1020D006
#endif
#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RW
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW  0x2020D006
#endif

#define LOAD_ADDRESS 0x98000000

// Definiciones GLOBALES de los módulos (sin extern)
so_module so_mod;
so_module so_mod_aux;

void __kuser_memory_barrier(void) {
    asm("dmb\n");
}

int __kuser_cmpxchg(int32_t oldval, int32_t newval, volatile int32_t *ptr) {
    return !atomic_compare_exchange_strong(ptr, &oldval, newval);
}

void soloader_init_all() {
	// Launch `app0:configurator.bin` on `-config` init param
    //sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    //SceAppUtilAppEventParam eventParam;
    //sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    //sceAppUtilReceiveAppEvent(&eventParam);
    //if (eventParam.type == 0x05) {
    //    char buffer[2048];
    //    sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
    //    if (strstr(buffer, "-config"))
    //        sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    //}

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("You need to install kubridge.skprx to play this game. "
                    "You can download it at https://github.com/bythos14/kubridge/releases");
    }

     // Checking for kubridge version
    char *kubridge_hash = file_sha1sum("ux0:/tai/kubridge.skprx");
    if (!kubridge_hash) kubridge_hash = file_sha1sum("ur0:/tai/kubridge.skprx");

    if (!kubridge_hash) {
        fatal_error("Could not find kubridge.skprx file despite the plugin "
                    "itself being active. Please put it in either ur0:/tai or "
                    "ux0:/tai folder.");
    }

    l_info("kubridge hash: %s", kubridge_hash);

    const char * ver_01 = "v0.1";
    const char * ver_02 = "v0.2";
    const char * ver_03 = "v0.3";

    const char *currently_installed_version = NULL;

    if      (strcmp(kubridge_hash, "E033D76A90C9B8F2D496735C2692AFD8C3ED32FE") == 0) // v0.1 (TheFloW)
    {
        currently_installed_version = ver_01;
    }
    else if (strcmp(kubridge_hash, "6CFC985904F9BBE3A4F54DD96197F5DF3E523DCB") == 0) // v0.2 (Bythos)
    {
        currently_installed_version = ver_02;
    }
    else if (strcmp(kubridge_hash, "AFAC6077618245D87CFF9ED2819223E6BB2DE5F8") == 0) // v0.3 (Bythos)
    {
        currently_installed_version = ver_03;
    }

    if (currently_installed_version) {
        free(kubridge_hash);
        l_fatal("kubridge check failed.");
        fatal_error("You need to update kubridge.skprx to version v0.3.1 or higher to play this game. "
                    "Currently installed version: %s. "
                    "You can download the update at https://github.com/bythos14/kubridge/releases",
                    currently_installed_version);
    }

    free(kubridge_hash);

    l_success("kubridge check passed.");


    // Verificar que existan ambas librerías
    if (!file_exists(SO_PATH_MAIN)) {
        l_fatal("Main SO could not be located.");
        fatal_error("Missing %s", SO_PATH_MAIN);
    }
    if (!file_exists(SO_PATH_AUX)) {
        l_fatal("Aux SO could not be located.");
        fatal_error("Missing %s", SO_PATH_AUX);
    }

    // Cargar primero la librería auxiliar (pequeña)
    if (so_file_load(&so_mod_aux, SO_PATH_AUX, LOAD_ADDRESS) < 0) {
        l_fatal("Aux SO could not be loaded.");
        fatal_error("Error loading %s", SO_PATH_AUX);
    }
    l_success("Aux SO loaded: %s", SO_PATH_AUX);

    // Luego cargar la librería principal
    if (so_file_load(&so_mod, SO_PATH_MAIN, LOAD_ADDRESS) < 0) {
        l_fatal("Main SO could not be loaded.");
        fatal_error("Error loading %s", SO_PATH_MAIN);
    }
    l_success("Main SO loaded: %s", SO_PATH_MAIN);

    // NOTA: ya no se necesita la comprobación de videos, la dejamos igual
    //if (!file_exists(DATA_PATH"data/briefing/Briefing_M01.mp4")) {
    //    l_fatal("Videos don't exist.");
    //    fatal_error("Error: please unpack videos into "DATA_PATH"data/briefing folder.", SO_PATH_MAIN);
    //}

    settings_load();
    l_success("Settings loaded.");


    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
    opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
    opt.attr = 0x1;
    opt.field_C = (SceUInt32)0x9A000000;
    if (kuKernelAllocMemBlock("atomic", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &opt) < 0)
        fatal_error("Error could not allocate atomic block.");

    hook_addr(0x9A000FA0, (uintptr_t)__kuser_memory_barrier);
    hook_addr(0x9A000FC0, (uintptr_t)__kuser_cmpxchg);


    // Relocalizar ambas
    so_relocate(&so_mod_aux);
    so_relocate(&so_mod);
    l_success("Both SOs relocated.");

    // Resolver importaciones de ambas
    resolve_imports(&so_mod_aux);
    resolve_imports(&so_mod);
    l_success("Both SOs imports resolved.");

    // Aplicar parches (todavía usa so_mod, luego lo ajustaremos)
    so_patch();
    l_success("SO patched.");

    // Flush caches de ambas
    so_flush_caches(&so_mod_aux);
    so_flush_caches(&so_mod);
    l_success("Caches flushed.");

    // Inicializar ambas
    so_initialize(&so_mod_aux);
    so_initialize(&so_mod);
    l_success("Both SOs initialized.");

    // El resto sigue igual
    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");

    gl_init();
    l_success("OpenGL initialized.");


    if (trophies_init() < 0) {
        warning("This game features unlockable trophies but NoTrpDrm is not installed. If you want to be able to unlock trophies, please install it.");
        l_warn("Trophies initialization failed.");
    } else {
        l_success("Trophies initialized.");
    }
}
