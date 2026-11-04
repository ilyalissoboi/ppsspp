package org.ppsspp.ppsspp;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Build;
import android.util.Log;

import java.util.HashSet;

// Tells native code when the device has an internet connection, so RetroAchievements can hold on to
// unlocks while it's offline, and submit them as soon as the connection is back.
public class NetworkConnectivityMonitor {
	private static final String TAG = NetworkConnectivityMonitor.class.getSimpleName();

	private final ConnectivityManager connectivityManager;
	private ConnectivityManager.NetworkCallback callback;
	// Connected networks that offer internet access.
	private final HashSet<Network> connectedNetworks = new HashSet<>();
	// Those of them the system has actually reached the internet through (no captive portal, etc).
	private final HashSet<Network> validatedNetworks = new HashSet<>();
	private Boolean lastSentOnline = null;

	public NetworkConnectivityMonitor(Context context) {
		connectivityManager = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
		if (connectivityManager == null) {
			return;
		}

		callback = new ConnectivityManager.NetworkCallback() {
			@Override
			public void onAvailable(Network network) {
				// Before Android 8, onCapabilitiesChanged doesn't necessarily follow.
				update(network, connectivityManager.getNetworkCapabilities(network));
			}

			@Override
			public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
				update(network, caps);
			}

			@Override
			public void onLost(Network network) {
				update(network, null);
			}
		};

		try {
			NetworkRequest request = new NetworkRequest.Builder()
				.addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
				.build();
			connectivityManager.registerNetworkCallback(request, callback);
		} catch (Exception e) {
			// Can throw if an app has registered too many callbacks.
			Log.e(TAG, "Failed to register network callback: " + e);
			callback = null;
			return;
		}

		// If there's no network at all, no callback comes.
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && connectivityManager.getActiveNetwork() == null) {
			send(false);
		}
	}

	public void destroy() {
		if (callback != null) {
			try {
				connectivityManager.unregisterNetworkCallback(callback);
			} catch (Exception e) {
				Log.e(TAG, "Failed to unregister network callback: " + e);
			}
			callback = null;
		}
	}

	// The callbacks come in on a ConnectivityManager thread. caps is null for a lost network.
	private synchronized void update(Network network, NetworkCapabilities caps) {
		boolean connected = caps != null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
		boolean validated = connected;
		if (connected && Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
			validated = caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED);
		}

		if (connected) {
			connectedNetworks.add(network);
		} else {
			connectedNetworks.remove(network);
		}
		boolean newlyValidated = false;
		if (validated) {
			newlyValidated = validatedNetworks.add(network);
		} else {
			validatedNetworks.remove(network);
		}

		// Only no network at all counts as offline: validation can take a while, or never happen on some
		// networks that work fine. A network that just validated is worth another try at submitting, though.
		boolean online = !connectedNetworks.isEmpty();
		if (newlyValidated || lastSentOnline == null || lastSentOnline != online) {
			send(online);
		}
	}

	private synchronized void send(boolean online) {
		if (!PpssppActivity.libraryLoaded) {
			return;
		}
		lastSentOnline = online;
		Log.i(TAG, "Network " + (online ? "available" : "unavailable"));
		try {
			NativeApp.sendMessageFromJava("network_connectivity", online ? "1" : "0");
		} catch (Exception e) {
			Log.e(TAG, "Exception in send: " + e);
		}
	}
}
