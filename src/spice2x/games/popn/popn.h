#pragma once

#include "games/game.h"

// TODO
#define POPN_PIKA_ENABLE 0
#define POPN_PIKA_SUB_WINDOW " sub"

namespace games::popn {

    class POPNGame : public games::Game {
    public:
        POPNGame();
        ~POPNGame() override;
        virtual void pre_attach() override;
        virtual void attach() override;
    };
    
#if POPN_PIKA_ENABLE

        // according to bemaniwiki,
        // J:A:A popn19/20 cabinets
        // J:B:A SD cabinets (popn18 and below)
        // J:C:A HD cabinets (popn21+)
        // J:D:A pikapika (popn HC)
        static inline bool is_pikapika_model() {
            return (
                avs::game::is_model("M39") && avs::game::SPEC[0] == 'D'
                );
        }

#else

        static inline bool is_pikapika_model() {
            return false;
        }

#endif

}
