package dev.teton.somniel;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/**
 * Restarts the game. Runs in its own process (":restart" in the manifest): it kills the game's process, then starts
 * SomnielActivity again, which boots the game from scratch. (A process cannot relaunch itself and die in one step.)
 */
public class RestartActivity extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Intent in = getIntent();
        int pid = in.getIntExtra("pid", -1);
        if (pid > 0) Process.killProcess(pid);
        Intent main = new Intent(this, SomnielActivity.class);
        main.setAction(Intent.ACTION_MAIN);
        main.addCategory(Intent.CATEGORY_LAUNCHER);
        main.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(main);
        finish();
        overridePendingTransition(0, 0);
        Process.killProcess(Process.myPid()); // this helper process has nothing left to do
    }
}
