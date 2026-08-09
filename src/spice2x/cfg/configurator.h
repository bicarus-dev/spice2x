#pragma once

#include "configurator_wnd.h"

namespace cfg {

    // globals
    extern bool CONFIGURATOR_STANDALONE;
    extern bool CONFIGURATOR_FORCE_SOFTWARE_RENDER;

    class Configurator {
    private:
        ConfiguratorWindow wnd;

    public:

        Configurator();
        ~Configurator();
        void run();
    };
}
