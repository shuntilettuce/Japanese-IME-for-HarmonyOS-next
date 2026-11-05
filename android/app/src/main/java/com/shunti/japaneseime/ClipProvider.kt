package com.shunti.japaneseime

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.File

/**
 * クリップボードの履歴の写し (ClipStore) を、貼り付け先のアプリに読ませるだけの入口。
 * exported=false で、読めるのは貼り付けのときに権限を渡したアプリだけ (commitContent・クリップボード)。読むだけで、書けない
 * (androidx の FileProvider を入れずに済むよう自前で)
 */
class ClipProvider : ContentProvider() {

    override fun onCreate() = true

    private fun fileOf(uri: Uri): File? {
        val name = uri.lastPathSegment ?: return null
        if (name.contains('/') || name.contains("..") || name.endsWith(".json")) return null
        val dir = ClipStore.get(context ?: return null).dir
        return File(dir, name).takeIf { it.isFile && it.parentFile == dir }
    }

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? {
        if (mode != "r") throw SecurityException("read only")
        val f = fileOf(uri) ?: throw java.io.FileNotFoundException(uri.toString())
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun getType(uri: Uri): String? {
        val name = uri.lastPathSegment ?: return null
        val c = context?.let { ctx -> ClipStore.get(ctx).list().firstOrNull { it.file == name } }
        return c?.mime ?: android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substringAfterLast('.', ""))
    }

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, args: Array<out String>?, sort: String?): Cursor? {
        val f = fileOf(uri) ?: return null
        val c = context?.let { ctx -> ClipStore.get(ctx).list().firstOrNull { it.file == f.name } }
        val cols = projection ?: arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)
        return MatrixCursor(cols).apply {
            addRow(cols.map { col ->
                when (col) {
                    OpenableColumns.DISPLAY_NAME -> c?.name?.ifEmpty { null } ?: f.name
                    OpenableColumns.SIZE -> f.length()
                    else -> null
                }
            }.toTypedArray())
        }
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun delete(uri: Uri, selection: String?, args: Array<out String>?) = 0
    override fun update(uri: Uri, values: ContentValues?, selection: String?, args: Array<out String>?) = 0
}
