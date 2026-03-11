// MoltBrowser AI Sample Extension — Background Service Worker
// Copyright 2025 GenEye AI Labs Inc.
//
// This demonstrates how to use the browser.ai.* Extension API
// introduced by MoltBrowser. Extensions need the "ai" permission
// in manifest.json to access these APIs.

// ============================================================
// browser.ai.run() — Run a prompt against local LLM
// ============================================================
async function runPrompt(prompt, options = {}) {
  try {
    const response = await browser.ai.run(prompt, {
      model: options.model || 'llama3.1-8b',
      maxTokens: options.maxTokens || 512,
      temperature: options.temperature || 0.7,
      stream: false,
      ...options
    });
    console.log('AI Response:', response.text);
    console.log('Model used:', response.model);
    console.log('Tokens:', response.tokens);
    return response;
  } catch (error) {
    console.error('AI run failed:', error);
    throw error;
  }
}

// ============================================================
// browser.ai.pageSummary() — Summarize the current page
// ============================================================
async function summarizeCurrentPage(tabId) {
  try {
    const summary = await browser.ai.pageSummary(tabId);
    console.log('Page Summary:', summary.summary);
    return summary;
  } catch (error) {
    console.error('Page summary failed:', error);
    throw error;
  }
}

// ============================================================
// browser.ai.agentTask() — Run an autonomous agent task
// ============================================================
async function runAgentTask(goal, options = {}) {
  try {
    const result = await browser.ai.agentTask(goal, {
      maxIterations: options.maxIterations || 20,
      timeout: options.timeout || 120000,
      allowNavigation: options.allowNavigation !== false,
      ...options
    });
    console.log('Agent Result:', result.answer);
    console.log('Steps taken:', result.steps);
    return result;
  } catch (error) {
    console.error('Agent task failed:', error);
    throw error;
  }
}

// ============================================================
// browser.ai.extractData() — Extract structured data from page
// ============================================================
async function extractPageData(tabId, selector) {
  try {
    const data = await browser.ai.extractData(selector);
    console.log('Extracted data:', data);
    return data;
  } catch (error) {
    console.error('Data extraction failed:', error);
    throw error;
  }
}

// ============================================================
// browser.ai.getModels() — List available models
// ============================================================
async function listModels() {
  try {
    const models = await browser.ai.getModels();
    console.log('Available models:', models);
    return models;
  } catch (error) {
    console.error('Get models failed:', error);
    throw error;
  }
}

// ============================================================
// browser.ai.embed() — Get text embedding vector
// ============================================================
async function getEmbedding(text) {
  try {
    const embedding = await browser.ai.embed(text);
    console.log('Embedding dimensions:', embedding.length);
    return embedding;
  } catch (error) {
    console.error('Embedding failed:', error);
    throw error;
  }
}

// ============================================================
// Event listener — Model loaded notification
// ============================================================
if (browser.ai && browser.ai.onModelLoaded) {
  browser.ai.onModelLoaded.addListener((modelInfo) => {
    console.log('Model loaded:', modelInfo.id, modelInfo.name);
  });
}

// ============================================================
// Message handler — Respond to popup requests
// ============================================================
chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  switch (message.action) {
    case 'summarize':
      summarizeCurrentPage(message.tabId)
        .then(result => sendResponse({ success: true, data: result }))
        .catch(error => sendResponse({ success: false, error: error.message }));
      return true; // Keep channel open for async response

    case 'ask':
      runPrompt(message.prompt, message.options)
        .then(result => sendResponse({ success: true, data: result }))
        .catch(error => sendResponse({ success: false, error: error.message }));
      return true;

    case 'extract':
      extractPageData(message.tabId, message.selector)
        .then(result => sendResponse({ success: true, data: result }))
        .catch(error => sendResponse({ success: false, error: error.message }));
      return true;

    case 'models':
      listModels()
        .then(result => sendResponse({ success: true, data: result }))
        .catch(error => sendResponse({ success: false, error: error.message }));
      return true;

    default:
      sendResponse({ success: false, error: 'Unknown action' });
  }
});
