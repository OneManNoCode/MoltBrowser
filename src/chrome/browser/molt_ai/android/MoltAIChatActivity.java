// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Android entry point for MoltBrowser AI Chat.
// Hosts a WebView pointed at chrome://molt-ai-chat, providing the same
// AI chat interface as the desktop side panel but in a full-screen Activity.
//
// Integration points:
//   - Launched from the toolbar AI button or Omnibox ai: prefix
//   - Can be opened as an overlay (bottom sheet) or standalone activity
//   - Communicates with BrowserAIRuntime via chrome.send() WebUI bridge

package com.geneye.moltbrowser.ai;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

/**
 * Full-screen AI Chat Activity for MoltBrowser Android.
 *
 * <p>This activity hosts the AI chat WebUI (chrome://molt-ai-chat) in a
 * WebView. It provides:
 * <ul>
 *   <li>On-device LLM inference via the native BrowserAIRuntime</li>
 *   <li>Page context awareness (current tab URL + selected text)</li>
 *   <li>Model download management</li>
 *   <li>Chat history and export</li>
 * </ul>
 *
 * <p>On Android, the AI chat runs as a separate Activity rather than a
 * side panel (which requires desktop views framework). The WebUI is
 * identical — same HTML/CSS/JS rendered in a WebView.
 */
public class MoltAIChatActivity extends Activity {

    private static final String CHAT_URL = "chrome://molt-ai-chat";
    private static final String EXTRA_PAGE_URL = "com.geneye.moltbrowser.PAGE_URL";
    private static final String EXTRA_SELECTED_TEXT = "com.geneye.moltbrowser.SELECTED_TEXT";

    private WebView mWebView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Edge-to-edge display
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(
            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
        );

        // Create WebView programmatically
        mWebView = new WebView(this);
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT
        );
        mWebView.setLayoutParams(params);

        // Configure WebView settings for AI chat
        WebSettings settings = mWebView.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setDatabaseEnabled(true);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);

        // Handle navigation within the WebView
        mWebView.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, String url) {
                // Keep chrome:// URLs in this WebView
                if (url.startsWith("chrome://")) {
                    return false;
                }
                // Open external URLs in the main browser
                Intent intent = new Intent(Intent.ACTION_VIEW);
                intent.setData(android.net.Uri.parse(url));
                startActivity(intent);
                return true;
            }
        });

        setContentView(mWebView);

        // Pass page context from the launching tab
        String pageUrl = getIntent().getStringExtra(EXTRA_PAGE_URL);
        String selectedText = getIntent().getStringExtra(EXTRA_SELECTED_TEXT);

        // Load the AI chat WebUI
        String chatUrl = CHAT_URL;
        if (pageUrl != null) {
            chatUrl += "?pageUrl=" + android.net.Uri.encode(pageUrl);
        }
        if (selectedText != null) {
            chatUrl += (pageUrl != null ? "&" : "?")
                + "selectedText=" + android.net.Uri.encode(selectedText);
        }
        mWebView.loadUrl(chatUrl);
    }

    @Override
    public void onBackPressed() {
        if (mWebView.canGoBack()) {
            mWebView.goBack();
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onDestroy() {
        if (mWebView != null) {
            mWebView.destroy();
        }
        super.onDestroy();
    }

    /**
     * Helper to launch AI chat from any context with page info.
     */
    public static Intent createIntent(Activity from, String pageUrl, String selectedText) {
        Intent intent = new Intent(from, MoltAIChatActivity.class);
        if (pageUrl != null) {
            intent.putExtra(EXTRA_PAGE_URL, pageUrl);
        }
        if (selectedText != null) {
            intent.putExtra(EXTRA_SELECTED_TEXT, selectedText);
        }
        return intent;
    }
}
