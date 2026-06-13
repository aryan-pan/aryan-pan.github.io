/*
 * site.js — UI behaviors for the portfolio.
 *  - hide/show header on scroll
 *  - active nav link
 *  - scroll-reveal (IntersectionObserver)
 *  - subtle mouse parallax/tilt on [data-tilt] elements
 *  - gallery lightbox
 * All motion respects prefers-reduced-motion.
 */
(function () {
  'use strict';
  var reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  /* ---- active nav link ---- */
  function normalize(p) { return (p || '/').replace(/index\.html$/, '').replace(/\/+$/, '') || '/'; }
  var here = normalize(location.pathname);
  document.querySelectorAll('.nav a[href]').forEach(function (a) {
    var path = normalize(new URL(a.href, location.origin).pathname);
    if (path === here && !a.classList.contains('brand')) a.classList.add('active');
  });

  /* ---- mobile nav toggle ---- */
  var navEl = document.querySelector('.nav');
  var navToggle = document.querySelector('.nav-toggle');
  if (navEl && navToggle) {
    function setOpen(open) {
      navEl.classList.toggle('open', open);
      navToggle.setAttribute('aria-expanded', open ? 'true' : 'false');
    }
    navToggle.addEventListener('click', function (e) {
      e.stopPropagation();
      setOpen(!navEl.classList.contains('open'));
    });
    navEl.querySelectorAll('.nav-links a').forEach(function (a) {
      a.addEventListener('click', function () { setOpen(false); });
    });
    document.addEventListener('click', function (e) {
      if (navEl.classList.contains('open') && !navEl.contains(e.target)) setOpen(false);
    });
    document.addEventListener('keydown', function (e) { if (e.key === 'Escape') setOpen(false); });
  }

  /* ---- hide header on scroll down, reveal on scroll up ---- */
  var header = document.querySelector('.site-header');
  if (header) {
    var lastY = window.scrollY, ticking = false;
    window.addEventListener('scroll', function () {
      if (ticking) return;
      ticking = true;
      requestAnimationFrame(function () {
        var y = window.scrollY;
        if (y > lastY && y > 120) header.classList.add('hide');
        else header.classList.remove('hide');
        lastY = y; ticking = false;
      });
    }, { passive: true });
  }

  /* ---- scroll reveal ---- */
  var reveals = document.querySelectorAll('.reveal');
  if (reduced || !('IntersectionObserver' in window)) {
    reveals.forEach(function (el) { el.classList.add('in'); });
  } else {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) {
        if (en.isIntersecting) { en.target.classList.add('in'); io.unobserve(en.target); }
      });
    }, { threshold: 0.12, rootMargin: '0px 0px -8% 0px' });
    reveals.forEach(function (el) { io.observe(el); });
  }

  /* ---- mouse parallax / tilt ---- */
  if (!reduced && window.matchMedia('(pointer:fine)').matches) {
    var tilts = Array.prototype.slice.call(document.querySelectorAll('[data-tilt]'));
    if (tilts.length) {
      var mx = 0, my = 0, cx = 0, cy = 0, raf = null;
      window.addEventListener('pointermove', function (e) {
        mx = e.clientX / window.innerWidth - 0.5;
        my = e.clientY / window.innerHeight - 0.5;
        if (!raf) raf = requestAnimationFrame(loop);
      }, { passive: true });
      function loop() {
        cx += (mx - cx) * 0.08; cy += (my - cy) * 0.08;
        tilts.forEach(function (el) {
          var depth = parseFloat(el.getAttribute('data-tilt')) || 1;
          var tx = -cx * 14 * depth, ty = -cy * 14 * depth;
          var rx = cy * 3 * depth, ry = -cx * 3 * depth;
          el.style.transform = 'perspective(900px) translate3d(' + tx.toFixed(2) + 'px,' + ty.toFixed(2) + 'px,0) rotateX(' + rx.toFixed(2) + 'deg) rotateY(' + ry.toFixed(2) + 'deg)';
        });
        if (Math.abs(mx - cx) > 0.001 || Math.abs(my - cy) > 0.001) raf = requestAnimationFrame(loop);
        else raf = null;
      }
    }
  }

  /* ---- gallery lightbox ---- */
  var gallery = document.querySelector('.gallery');
  if (gallery) {
    var lb = document.createElement('div');
    lb.className = 'lightbox';
    lb.innerHTML = '<button class="lightbox-close" aria-label="Close">&times;</button><img alt="">';
    document.body.appendChild(lb);
    var lbImg = lb.querySelector('img');
    gallery.addEventListener('click', function (e) {
      var img = e.target.closest('img');
      if (!img) return;
      lbImg.src = img.currentSrc || img.src;
      lbImg.alt = img.alt || '';
      lb.classList.add('open');
    });
    function close() { lb.classList.remove('open'); lbImg.src = ''; }
    lb.addEventListener('click', function (e) { if (e.target === lb || e.target.classList.contains('lightbox-close')) close(); });
    document.addEventListener('keydown', function (e) { if (e.key === 'Escape') close(); });
  }
})();
