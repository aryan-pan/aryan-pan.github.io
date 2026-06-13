# Blog CMS setup (Sveltia CMS)

You write posts at **https://aryan-pan.github.io/admin/**. Sveltia logs you in with
GitHub and commits a markdown file to `_posts/`; GitHub Pages rebuilds (~1 min) and
the post is live.

GitHub Pages is static, so login needs a tiny OAuth relay. The free, recommended one
is a **Cloudflare Worker** (`sveltia-cms-auth`). One-time setup, ~10 minutes.

---

## 1. Deploy the Cloudflare auth worker

1. Make a free account at https://dash.cloudflare.com if you don't have one.
2. Open **https://github.com/sveltia/sveltia-cms-auth** and click the
   **“Deploy to Cloudflare”** button in its README (or: Cloudflare dashboard →
   *Workers & Pages* → *Create* → *Import a repository* → that repo).
3. After it deploys, copy the worker URL, e.g. `https://sveltia-cms-auth.<you>.workers.dev`.

## 2. Create a GitHub OAuth app

1. Go to https://github.com/settings/developers → **OAuth Apps** → **New OAuth App**.
2. Fill in:
   - **Application name:** `aryan-pan.github.io CMS`
   - **Homepage URL:** `https://aryan-pan.github.io`
   - **Authorization callback URL:** `https://<your-worker-url>/callback`
3. **Register application.** Copy the **Client ID**, then **Generate a new client secret**
   and copy it (you only see it once).

## 3. Give the worker your OAuth credentials

In the Cloudflare dashboard → your worker → **Settings → Variables and Secrets**, add:

| Name                   | Value                                   | Type   |
| ---------------------- | --------------------------------------- | ------ |
| `GITHUB_CLIENT_ID`     | the Client ID from step 2               | Text   |
| `GITHUB_CLIENT_SECRET` | the Client Secret from step 2           | Secret |
| `ALLOWED_DOMAINS`      | `aryan-pan.github.io`                    | Text   |

Save and (re)deploy the worker.

## 4. Point the CMS at the worker

Edit **`admin/config.yml`** in this repo — change:

```yaml
base_url: https://REPLACE-ME.workers.dev
```

to your real worker URL (no trailing slash, no `/callback`). Commit and push.

## 5. Use it

- Go to **https://aryan-pan.github.io/admin/** → **Sign in with GitHub** → authorize.
- Click **Blog posts → New Post**, write, and **Publish**. It commits to `_posts/`
  and the site rebuilds in about a minute.

---

## Local testing (optional, no worker needed)

Sveltia can edit your local files directly in a Chromium browser (Chrome/Edge):

1. `jekyll serve` (already running on http://localhost:4000)
2. Open **http://localhost:4000/admin/**
3. Click **“Work with Local Repository”**, pick this repo's folder, and edit.
   Changes write straight to your local `_posts/` — great for trying it out before
   the worker is set up.
