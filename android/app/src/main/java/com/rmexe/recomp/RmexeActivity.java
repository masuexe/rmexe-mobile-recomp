package com.rmexe.recomp;

import android.content.pm.PackageManager;
import org.libsdl.app.SDLActivity;

public class RmexeActivity extends SDLActivity {
    private static final String BLOCKED_PACKAGE = "net.n1gp.nettai";

    private boolean isNettaiInstalled() {
        if (BLOCKED_PACKAGE.equals(getPackageName())) {
            return false;
        }
        try {
            getPackageManager().getPackageInfo(BLOCKED_PACKAGE, 0);
            return true;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    @Override
    public void loadLibraries() {
        if (isNettaiInstalled()) {
            throw new SecurityException(
                "Startup blocked: detected installed package '" + BLOCKED_PACKAGE + "'. "
                    + "Please completely uninstall nettai and try again.\n"
                    + "启动已阻止：检测到本机已安装软件包 '" + BLOCKED_PACKAGE + "'，请彻底卸载 nettai 后重试。"
            );
        }
        super.loadLibraries();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "c++_shared",
            "SDL2",
            "main"
        };
    }
}
