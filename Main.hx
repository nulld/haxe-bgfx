
import bgfx.Bgfx;

class Main {
    public static function main() {
        trace("0");

        bgfx.Bgfx.init();
        
        while(true) {
            bgfx.Bgfx.clearView(0xff0000ff);
        }
        trace("1");
    }
}