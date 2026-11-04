// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The fixture shop's cart (shop.test), in sessionStorage.
'use strict';

window.shop = {
  items() {
    try {
      return JSON.parse(sessionStorage.getItem('cart') || '[]');
    } catch (e) {
      return [];
    }
  },
  add(name) {
    const items = shop.items();
    items.push(name);
    sessionStorage.setItem('cart', JSON.stringify(items));
    shop.show();
  },
  clear() {
    sessionStorage.removeItem('cart');
    shop.show();
  },
  show() {
    const link = document.getElementById('cart-link');
    if (link) {
      link.textContent = 'Cart (' + shop.items().length + ')';
    }
  },
};
