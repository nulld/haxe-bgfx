package bgfx;

@:include('../../linc/linc_bgfx.h')
#if !display
@:build(linc.Linc.touch())
@:build(linc.Linc.xml('bgfx'))
#end
extern class Bgfx {
    @:native("linc::bgfx::init") public static function init():Void;
    @:native("linc::bgfx::clearView") public static function clearView(rgba:UInt):Void;
}
