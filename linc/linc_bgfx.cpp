//hxcpp include should be first
#include <hxcpp.h>
#include "./linc_bgfx.h"

namespace linc {

    namespace bgfx {
        void init()
        {
            bgfx_init_t init;
            bgfx_init_ctor(&init);
            bgfx_init(&init);
            printf("bgfx init OK");
        }

        void clearView(uint32_t rgba)
        {
            bgfx_set_view_clear(0
		        , BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH
		        , rgba
		        , 1.0f
		        , 0
		    );
        }

    } //bgfx namespace

} //linc namespace