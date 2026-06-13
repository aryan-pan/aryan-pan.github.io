---
layout: default
title: Album
description: Photographs by Aryan Pandey — nature, rain, and the occasional lab bench.
---

<section class="section wrap">
  <div class="section-head reveal">
    <h2>Album</h2>
    <p class="muted">Nature, rain, and the occasional lab bench. Click any photo to enlarge.</p>
  </div>
  <div class="gallery reveal">
    {% for photo in site.data.album.photos %}
    <figure>
      <img src="{{ photo.image | relative_url }}" alt="{{ photo.caption | default: 'Photograph' }}" loading="lazy">
      {% if photo.caption %}<figcaption>{{ photo.caption }}</figcaption>{% endif %}
    </figure>
    {% endfor %}
  </div>
  {% unless site.data.album.photos %}<p class="muted reveal">No photos yet.</p>{% endunless %}
</section>
