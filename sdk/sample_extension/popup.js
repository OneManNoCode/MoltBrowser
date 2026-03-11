// MoltBrowser AI Sample Extension — Popup Script
// Copyright 2025 GenEye AI Labs Inc.

const resultDiv = document.getElementById('result');
const statusDiv = document.getElementById('status');
const promptInput = document.getElementById('prompt');

function showResult(text) {
  resultDiv.textContent = text;
  resultDiv.style.display = 'block';
}

function showStatus(text) {
  statusDiv.textContent = text;
}

// Ask AI button
document.getElementById('askBtn').addEventListener('click', async () => {
  const prompt = promptInput.value.trim();
  if (!prompt) return;

  showStatus('Thinking...');
  showResult('');

  chrome.runtime.sendMessage(
    { action: 'ask', prompt: prompt },
    (response) => {
      if (response.success) {
        showResult(response.data.text);
        showStatus(`Model: ${response.data.model} | Tokens: ${response.data.tokens}`);
      } else {
        showResult('Error: ' + response.error);
        showStatus('');
      }
    }
  );
});

// Summarize Page button
document.getElementById('summarizeBtn').addEventListener('click', async () => {
  showStatus('Summarizing page...');
  showResult('');

  chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
    chrome.runtime.sendMessage(
      { action: 'summarize', tabId: tabs[0].id },
      (response) => {
        if (response.success) {
          showResult(response.data.summary);
          showStatus('Page summarized');
        } else {
          showResult('Error: ' + response.error);
          showStatus('');
        }
      }
    );
  });
});

// Models button
document.getElementById('modelsBtn').addEventListener('click', async () => {
  showStatus('Loading models...');

  chrome.runtime.sendMessage(
    { action: 'models' },
    (response) => {
      if (response.success) {
        const models = response.data;
        let text = 'Available Models:\n\n';
        models.forEach(m => {
          const status = m.loaded ? 'LOADED' : (m.downloaded ? 'Ready' : 'Download');
          const size = (m.size_bytes / 1e9).toFixed(1) + 'GB';
          text += `${m.name} (${size}) [${status}]\n`;
        });
        showResult(text);
        showStatus(`${models.length} models available`);
      } else {
        showResult('Error: ' + response.error);
        showStatus('');
      }
    }
  );
});

// Enter key to submit
promptInput.addEventListener('keypress', (e) => {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    document.getElementById('askBtn').click();
  }
});
