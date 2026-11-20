package com.rmexe.recomp;

import org.libsdl.app.SDLActivity;

public class RmexeActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL2",
            "main"
        };
    }
}
