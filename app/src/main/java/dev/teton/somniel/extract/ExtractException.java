package dev.teton.somniel.extract;

import java.io.IOException;

/** A problem with the user's files that the setup screen can explain (missing key, wrong file, unsupported ticket). */
public class ExtractException extends IOException {
    public ExtractException(String message) {
        super(message);
    }
}
