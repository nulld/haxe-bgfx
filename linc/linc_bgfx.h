#pragma once

#ifndef HXCPP_H
#include <hxcpp.h>
#endif
#include "../lib/bgfx.h"

namespace linc {

    namespace bgfx {

        void init();
        void clearView(uint32_t rgba);

    } //bgfx namespace

} //linc namespace