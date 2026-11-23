package com.gblbds.manager.root

import java.io.File
import java.util.concurrent.TimeUnit

/** Root shell: one `su` process per script, with stdout/stderr drained. */
class RootShell {
    data class Result(val code: Int, val out: String, val err: String) {
        val ok: Boolean get() = code == 0
    }

    fun available(): Boolean {
        val r = run("id -u")
        return r.ok && r.out.trim() == "0"
    }

    fun run(script: String, timeoutMs: Long = 120_000): Result {
        return try {
            val p = Runtime.getRuntime().exec(arrayOf("su", "-c", script))
            val outBox = arrayOf("")
            val errBox = arrayOf("")
            val tOut = Thread { outBox[0] = p.inputStream.bufferedReader().readText() }
            val tErr = Thread { errBox[0] = p.errorStream.bufferedReader().readText() }
            tOut.start()
            tErr.start()
            val finished = p.waitFor(timeoutMs, TimeUnit.MILLISECONDS)
            if (!finished) {
                p.destroyForcibly()
                return Result(124, "", "timeout: $script")
            }
            tOut.join(2000)
            tErr.join(2000)
            Result(p.exitValue(), outBox[0], errBox[0])
        } catch (e: Exception) {
            Result(-1, "", e.toString())
        }
    }

    private fun q(s: String) = "'" + s.replace("'", "'\\''") + "'"

    /** Push an app-private file to a root-owned path. */
    fun copyAsRoot(src: File, dst: String): Result =
        run("mkdir -p ${q(dst.substringBeforeLast('/'))} && " +
            "cp ${q(src.absolutePath)} ${q(dst)} && chmod 0755 ${q(dst)}")
}
