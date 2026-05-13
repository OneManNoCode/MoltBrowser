---
date: 2026-05-12
title: A history page that knows what you were reading, not just where you clicked
slug: grouped-history
tags: [memory, history, clustering]
---

Every browser ships a History page. Every browser's History page is
the same: a reverse-chronological list of URLs. You scroll, your eyes
glaze over, you give up, you re-google.

The problem isn't memory — the browser remembers everything. The
problem is that a flat list is the worst possible affordance for
"the thing I was reading on Tuesday about REITs."

We already have **Personal Vector Memory** — the encrypted on-device
semantic index that we built into MoltBrowser a few days ago. Every
page you visit gets chunked, hashed-embedded, encrypted, and indexed.
Today we put a face on it.

## Try it

Open the side panel, type `/history`. You get:

```
Pages in your local memory, grouped into 14 topics.

▾ Bitcoin · Ethereum                                            38
  > Why bitcoin spot ETFs matter for Coinbase    coindesk.com · 2h ago
  > Ethereum upgrade roadmap                      ethereum.org · 2h ago
  > BTC at $97k as institutional demand…          bloomberg.com · 5h ago
  …

▾ REIT · Dividend                                                12
  > VNQ vs. SCHH — which broad REIT ETF…          morningstar.com · 1d ago
  > Realty Income earnings beat                   wsj.com · 1d ago
  …

▸ Bali · Travel                                                  7
▸ Recipe · Pasta                                                 5
…
```

The top 3 clusters open by default; the rest are collapsed. Click any
URL to open the original page.

## How the clustering works

No LLM in the hot path. Just a Jaccard-overlap greedy clusterer over
title keywords, running entirely in the WebUI JS:

1. For each doc's title, build a set of stop-word-stripped tokens
   (length 3+, deduped).
2. For each doc, walk every existing cluster; compute
   `|A ∩ B| / |A ∪ B|`. Highest score above 0.20 wins; below the
   threshold, start a new cluster.
3. Cluster label = top-2 most-frequent tokens in its keyword bag.

Tunings:

- `0.20` threshold tuned by hand on a sample of ~200 docs. Lower
  values over-cluster (everything becomes "the · a · and"); higher
  values under-cluster (every article gets its own group).
- Top-2 token label feels right at first glance. Top-1 felt thin;
  top-3 felt cluttered.
- Largest clusters first. The most-frequent topic of your reading
  this week should be the first thing you see.

The whole thing runs in <50ms for 500 documents.

## Why it's local, and why that's the whole point

This is the kind of feature that, in any other browser, ends with
"sign in to enable AI features." There's nothing about clustering
your reading that requires a cloud — the docs are already indexed on
your machine, the embeddings are already on your machine, the cosine
search already runs in <1ms locally. There is **no reason** to ship
your reading history to anyone's server for this.

So we don't.

## What's next

- **Smart bookmarks**: same clusterer, scoped to bookmarks instead of
  all visited pages. "The article I bookmarked about Bali" becomes a
  search instead of a hunt.
- **Cluster Q&A**: click a cluster, ask "summarize this topic" — the
  LLM answers from the docs in that cluster only.
- **Cross-tab Q&A**: clusterer-but-on-open-tabs.

— GenEye AI Labs
