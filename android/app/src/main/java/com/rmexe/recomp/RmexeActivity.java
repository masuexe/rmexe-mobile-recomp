package com.rmexe.recomp;

import org.libsdl.app.SDLActivity;

public class RmexeActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "c++_shared",
            "SDL2",
            "main"
        };
    }
}
