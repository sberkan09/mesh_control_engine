package org.meshcontrol.billing;

import android.app.Activity;
import android.util.Log;

import com.android.billingclient.api.AcknowledgePurchaseParams;
import com.android.billingclient.api.BillingClient;
import com.android.billingclient.api.BillingClientStateListener;
import com.android.billingclient.api.BillingFlowParams;
import com.android.billingclient.api.BillingResult;
import com.android.billingclient.api.ConsumeParams;
import com.android.billingclient.api.PendingPurchasesParams;
import com.android.billingclient.api.ProductDetails;
import com.android.billingclient.api.Purchase;
import com.android.billingclient.api.PurchasesUpdatedListener;
import com.android.billingclient.api.QueryProductDetailsParams;
import com.android.billingclient.api.QueryPurchasesParams;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

// Play Billing for mce_platform's store (platform/store.h, M-MONETIZE step 3). Stamped into the
// Gradle project of a game with a store catalog (cmake/mobile/android_app.py), driven through
// MceActivity's mceBilling* methods over JNI. Every call hops to the UI thread; every answer goes
// back through nativeAnswer (store.cpp) with platform::store::AnswerKind's values.
//
// A purchase is reported Purchased with its token and stays unconsumed and unacknowledged until
// finish(): Play refunds a purchase left unacknowledged for three days, and one bought before a
// crash comes back from the query at the next start (request 0).
public final class MceBilling implements PurchasesUpdatedListener, BillingClientStateListener {
    static final int PRODUCT_INFO = 0;
    static final int PRODUCTS_READY = 1;
    static final int PURCHASED = 2;
    static final int PENDING = 3;
    static final int CANCELLED = 4;
    static final int FAILED = 5;
    static final int RESTORED = 6;
    static final int RESTORE_FINISHED = 7;

    static native void nativeAnswer(int kind, int request, String id, String price, String transaction);

    private static final String TAG = "MceBilling";

    private final Activity activity;
    private BillingClient client;
    private boolean ready;
    private boolean connecting;
    private final ArrayList<Runnable> waiting = new ArrayList<>();      // calls made before the connection
    private final ArrayList<Runnable> refusals = new ArrayList<>();     // their answers if it fails
    private final Map<String, ProductDetails> details = new HashMap<>();
    private final Map<String, Boolean> reported = new HashMap<>();      // purchase tokens already answered
    private final Map<String, Boolean> acknowledged = new HashMap<>();  // restored tokens Play already holds as done
    private int flowRequest;  // the purchase flow on screen (Play runs one at a time)
    private String flowId;

    public MceBilling(Activity activity) {
        this.activity = activity;
    }

    // ---- calls from MceActivity (SDL's thread) ---------------------------------------------------

    public void start() {
        activity.runOnUiThread(() -> {
            if (client == null) {
                client = BillingClient.newBuilder(activity)
                        .setListener(this)
                        .enablePendingPurchases(PendingPurchasesParams.newBuilder().enableOneTimeProducts().build())
                        .build();
            }
            // Purchases finished while the game was not looking (a crash, a pending payment).
            whenReady(() -> queryOwned(0, false), () -> {});
        });
    }

    public void stop() {
        activity.runOnUiThread(() -> {
            if (client != null) client.endConnection();
            client = null;
            ready = false;
            connecting = false;
            waiting.clear();
            refusals.clear();
        });
    }

    public void query(String[] ids, int request) {
        activity.runOnUiThread(() -> whenReady(() -> queryDetails(ids, request, null),
                () -> nativeAnswer(PRODUCTS_READY, request, null, null, null)));
    }

    public void purchase(String id, boolean consumable, int request) {
        activity.runOnUiThread(() -> whenReady(() -> {
            ProductDetails pd = details.get(id);
            if (pd != null) launch(pd, request);
            else queryDetails(new String[] { id }, 0, () -> {
                ProductDetails found = details.get(id);
                if (found != null) launch(found, request);
                else nativeAnswer(FAILED, request, id, null, null);
            });
        }, () -> nativeAnswer(FAILED, request, id, null, null)));
    }

    public void restore(int request) {
        activity.runOnUiThread(() -> whenReady(() -> queryOwned(request, true),
                () -> nativeAnswer(RESTORE_FINISHED, request, null, null, null)));
    }

    public void finish(String token, boolean consumable) {
        activity.runOnUiThread(() -> whenReady(() -> {
            if (consumable) {
                client.consumeAsync(ConsumeParams.newBuilder().setPurchaseToken(token).build(),
                        (result, t) -> logIfFailed("consume", result));
            } else if (acknowledged.remove(token) == null) {
                client.acknowledgePurchase(AcknowledgePurchaseParams.newBuilder().setPurchaseToken(token).build(),
                        result -> logIfFailed("acknowledge", result));
            }
        }, () -> {}));
    }

    // ---- Play's callbacks (UI thread) --------------------------------------------------------------

    @Override
    public void onBillingSetupFinished(BillingResult result) {
        connecting = false;
        ready = result.getResponseCode() == BillingClient.BillingResponseCode.OK;
        ArrayList<Runnable> run = new ArrayList<>(ready ? waiting : refusals);
        waiting.clear();
        refusals.clear();
        if (!ready) Log.w(TAG, "billing setup failed: " + result.getDebugMessage());
        for (Runnable r : run) r.run();
    }

    @Override
    public void onBillingServiceDisconnected() {
        ready = false;  // the next call reconnects
    }

    @Override
    public void onPurchasesUpdated(BillingResult result, List<Purchase> purchases) {
        int code = result.getResponseCode();
        int request = flowRequest;
        String id = flowId;
        flowRequest = 0;
        flowId = null;
        if (code == BillingClient.BillingResponseCode.OK && purchases != null) {
            for (Purchase p : purchases) report(p, request);
        } else if (code == BillingClient.BillingResponseCode.USER_CANCELED) {
            nativeAnswer(CANCELLED, request, id, null, null);
        } else if (code == BillingClient.BillingResponseCode.ITEM_ALREADY_OWNED) {
            queryOwned(request, false);  // the owned purchase answers this request
        } else {
            nativeAnswer(FAILED, request, id, null, null);
        }
    }

    // ---- helpers (UI thread) -----------------------------------------------------------------------

    private void whenReady(Runnable call, Runnable refuse) {
        if (client == null) {
            refuse.run();
            return;
        }
        if (ready) {
            call.run();
            return;
        }
        waiting.add(call);
        refusals.add(refuse);
        if (!connecting) {
            connecting = true;
            client.startConnection(this);
        }
    }

    private void queryDetails(String[] ids, int request, Runnable then) {
        ArrayList<QueryProductDetailsParams.Product> list = new ArrayList<>();
        for (String id : ids) {
            list.add(QueryProductDetailsParams.Product.newBuilder()
                    .setProductId(id)
                    .setProductType(BillingClient.ProductType.INAPP)
                    .build());
        }
        if (list.isEmpty()) {
            if (then != null) then.run();
            else nativeAnswer(PRODUCTS_READY, request, null, null, null);
            return;
        }
        client.queryProductDetailsAsync(QueryProductDetailsParams.newBuilder().setProductList(list).build(),
                (result, found) -> activity.runOnUiThread(() -> {
                    logIfFailed("product query", result);
                    if (found != null) {
                        for (ProductDetails pd : found) {
                            details.put(pd.getProductId(), pd);
                            ProductDetails.OneTimePurchaseOfferDetails offer = pd.getOneTimePurchaseOfferDetails();
                            if (then == null) {
                                nativeAnswer(PRODUCT_INFO, request, pd.getProductId(),
                                        offer != null ? offer.getFormattedPrice() : "", null);
                            }
                        }
                    }
                    if (then != null) then.run();
                    else nativeAnswer(PRODUCTS_READY, request, null, null, null);
                }));
    }

    private void launch(ProductDetails pd, int request) {
        if (flowId != null) {
            nativeAnswer(FAILED, request, pd.getProductId(), null, null);  // one purchase flow at a time
            return;
        }
        flowRequest = request;
        flowId = pd.getProductId();
        BillingFlowParams params = BillingFlowParams.newBuilder()
                .setProductDetailsParamsList(Collections.singletonList(
                        BillingFlowParams.ProductDetailsParams.newBuilder().setProductDetails(pd).build()))
                .build();
        BillingResult result = client.launchBillingFlow(activity, params);
        if (result.getResponseCode() != BillingClient.BillingResponseCode.OK) onPurchasesUpdated(result, null);
    }

    // restore: every owned product as Restored, then RestoreFinished. Otherwise (start, an
    // already-owned purchase): the purchases not yet finished, as Purchased.
    private void queryOwned(int request, boolean restore) {
        client.queryPurchasesAsync(
                QueryPurchasesParams.newBuilder().setProductType(BillingClient.ProductType.INAPP).build(),
                (result, purchases) -> activity.runOnUiThread(() -> {
                    logIfFailed("purchase query", result);
                    if (purchases != null) {
                        for (Purchase p : purchases) {
                            if (p.getPurchaseState() != Purchase.PurchaseState.PURCHASED) continue;
                            if (restore) {
                                if (p.isAcknowledged()) acknowledged.put(p.getPurchaseToken(), Boolean.TRUE);
                                for (String id : p.getProducts()) nativeAnswer(RESTORED, request, id, null, p.getPurchaseToken());
                            } else if (!p.isAcknowledged()) {
                                report(p, request);
                            }
                        }
                    }
                    if (restore) nativeAnswer(RESTORE_FINISHED, request, null, null, null);
                }));
    }

    private void report(Purchase p, int request) {
        for (String id : p.getProducts()) {
            if (p.getPurchaseState() == Purchase.PurchaseState.PURCHASED) {
                // The start-up query and Play's listener can both see one purchase: answer it once,
                // unless a request is waiting on it.
                if (request == 0 && reported.containsKey(p.getPurchaseToken())) continue;
                reported.put(p.getPurchaseToken(), Boolean.TRUE);
                nativeAnswer(PURCHASED, request, id, null, p.getPurchaseToken());
            } else if (p.getPurchaseState() == Purchase.PurchaseState.PENDING) {
                nativeAnswer(PENDING, request, id, null, null);
            }
        }
    }

    private static void logIfFailed(String what, BillingResult result) {
        if (result.getResponseCode() != BillingClient.BillingResponseCode.OK)
            Log.w(TAG, what + " failed (" + result.getResponseCode() + "): " + result.getDebugMessage());
    }
}
