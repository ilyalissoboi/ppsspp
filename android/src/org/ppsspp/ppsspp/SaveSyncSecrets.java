package org.ppsspp.ppsspp;

import android.content.Context;
import android.util.Log;

import io.github.ilyalissoboi.emusync.SyncSettings;

/**
 * Save sync's secrets (Core/SaveSync.h): the storage's secret access key and the optional
 * encryption passphrase, sealed with a key in the Android Keystore by emusync's SyncSettings,
 * which never leaves the device. The other save sync settings are in ppsspp.ini like any other.
 *
 * Called from native code (app-android.cpp), on any thread attached to the JVM, and only on
 * Android 7.0 and later: the emusync AAR's minimum, and the only versions PPSSPP syncs on.
 */
public final class SaveSyncSecrets {
	private static final String TAG = "PPSSPPSaveSync";
	// SaveSync's names for them.
	private static final String SECRET_ACCESS_KEY = "s3-secret-access-key";
	private static final String ENCRYPTION_PASSPHRASE = "encryption-passphrase";

	private static SyncSettings settings;

	private SaveSyncSecrets() {}

	private static synchronized SyncSettings get(Context context) {
		if (settings == null) {
			settings = new SyncSettings(context, "ppsspp_save_sync", "ppsspp.saveSync.secret");
		}
		return settings;
	}

	/** Stores a secret; an empty value removes it. Returns false if it couldn't be stored. */
	public static boolean store(Context context, String name, String value) {
		try {
			SyncSettings s = get(context);
			if (SECRET_ACCESS_KEY.equals(name)) {
				s.setSecretAccessKey(value);
			} else if (ENCRYPTION_PASSPHRASE.equals(name)) {
				s.setPassphrase(value);
			} else {
				return false;
			}
			return true;
		} catch (Exception e) {
			Log.e(TAG, "Couldn't store " + name + ": " + e);
			return false;
		}
	}

	/** The stored secret, or null when there is none (or it can't be decrypted on this device). */
	public static String load(Context context, String name) {
		try {
			SyncSettings s = get(context);
			if (SECRET_ACCESS_KEY.equals(name)) {
				return s.secretAccessKey();
			} else if (ENCRYPTION_PASSPHRASE.equals(name)) {
				return s.passphrase();
			}
			return null;
		} catch (Exception e) {
			Log.e(TAG, "Couldn't read " + name + ": " + e);
			return null;
		}
	}
}
