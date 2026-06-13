---
layout: default
title: Writing
permalink: /blog/
description: Writing by Aryan Pandey — notes on neuromorphic computing, hardware-efficient ML, and whatever else is on my mind.
---

<section class="section wrap">
  <div class="section-head reveal">
    <h2>Writing</h2>
    <p class="muted">Notes on neuromorphic computing, hardware-efficient ML, and the occasional tangent.</p>
  </div>
  <div class="post-list">
    {% for post in site.posts %}
      <a class="glass post-card reveal" href="{{ post.url | relative_url }}">
        <h3>{{ post.title }}</h3>
        <p class="post-meta">{{ post.date | date: "%B %-d, %Y" }}</p>
        <p class="muted">{{ post.description | default: post.excerpt | strip_html | truncate: 180 }}</p>
        <span class="read-more">Read →</span>
      </a>
    {% endfor %}
    {% if site.posts == empty %}
      <p class="muted">No posts yet — stay tuned.</p>
    {% endif %}
  </div>
</section>
