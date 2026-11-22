#!/bin/sh
# Publie le dépôt de paquets npk en ligne, sur GitHub Pages.
#
#   make publish            (après 'make packages' et 'make dist')
#   DRY_RUN=1 make publish  prépare le site dans .pages sans rien envoyer
#
# Copie dist/repo (INDEX signé + paquets .npk) dans un dépôt GitHub PUBLIC
# séparé, « nexus-paquets » (le code de Nexus OS peut rester privé), avec une
# page d'accueil index.html qui liste les paquets. La branche « main » de ce
# dépôt est remplacée à chaque publication (un seul commit) : son historique
# ne grossit pas à chaque version.
#   PAGES_REPO=https://github.com/<compte>/<dépôt>.git   pour en choisir un autre
#
# Une fois GitHub Pages activé (Settings → Pages → branche main, dossier /),
# le dépôt est servi à l'adresse https://<compte>.github.io/nexus-paquets/ et
# npk l'utilise automatiquement quand le Mac ne sert pas de dépôt
# (voir /etc/npk/repo.conf).
#
# Rien de secret n'est publié : seulement les paquets, l'INDEX et sa
# signature (la clé privée keys/nexus-npk.key reste sur le Mac).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$ROOT/dist/repo"
OUT="$ROOT/.pages"
. "$ROOT/config/versions.env"

[ -f "$REPO/INDEX" ] && [ -f "$REPO/INDEX.minisig" ] ||
    { echo "Dépôt absent ou non signé : lance d'abord 'make packages' puis 'make dist'" >&2; exit 1; }
ORIGIN=$(git -C "$ROOT" remote get-url origin 2>/dev/null) ||
    { echo "Pas de remote « origin » dans le dépôt git" >&2; exit 1; }
# même compte que le code : https://github.com/<compte>/nexus-paquets.git
REMOTE="${PAGES_REPO:-$(echo "$ORIGIN" | sed -E 's#(github.com[/:][^/]+)/.*#\1/nexus-paquets.git#')}"

echo "[nexus] Préparation du site ($(du -sh "$REPO" | cut -f1))"
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$REPO"/INDEX "$REPO"/INDEX.minisig "$REPO"/*.npk "$OUT"/
touch "$OUT/.nojekyll"                    # fichiers servis tels quels

# ---- Page d'accueil : la liste des paquets ----
html_escape() { sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'; }
NB=$(wc -l < "$REPO/INDEX" | tr -d ' ')
DATE=$(date '+%d/%m/%Y à %H:%M')
{
cat <<EOF
<!doctype html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Dépôt Nexus OS</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; font-family: system-ui, -apple-system, "DejaVu Sans", sans-serif;
         background: radial-gradient(circle at 20% 0%, #1b2a6b 0%, #0b1023 55%) fixed; color: #eef1f7; }
  main { max-width: 920px; margin: 0 auto; padding: 40px 16px 60px; }
  h1 { font-size: 30px; margin: 0 0 6px; }
  .sub { color: #9aa3b8; margin: 0 0 28px; }
  .card { background: rgba(255,255,255,.06); border: 1px solid rgba(255,255,255,.12);
          border-radius: 16px; padding: 18px 20px; margin-bottom: 22px; }
  code { background: rgba(0,0,0,.35); padding: 2px 6px; border-radius: 6px; }
  pre { background: rgba(0,0,0,.35); padding: 12px 14px; border-radius: 10px; overflow-x: auto; }
  table { width: 100%; border-collapse: collapse; font-size: 15px; }
  th, td { text-align: left; padding: 9px 8px; border-bottom: 1px solid rgba(255,255,255,.08); vertical-align: top; }
  th { color: #9aa3b8; font-weight: 600; }
  td.v, td.s { white-space: nowrap; color: #b9c1d3; }
  .logo { display: inline-block; width: 40px; height: 40px; border-radius: 10px; vertical-align: -9px;
          background: linear-gradient(135deg, #22d3ee, #1e6fe0); color: #fff; font-weight: 800;
          text-align: center; line-height: 40px; margin-right: 10px; }
</style>
</head>
<body><main>
<h1><span class="logo">N</span>Dépôt de paquets Nexus OS</h1>
<p class="sub">Version $NEXUS_VERSION · $NB paquets · mis à jour le $DATE</p>
<div class="card">
  <p>Ce site est le dépôt <strong>npk</strong> de Nexus OS. Dans Nexus OS, il est utilisé
  automatiquement : Magasin d'applications, ou dans un terminal :</p>
  <pre>su -c "npk update"
npk search
su -c "npk install nano"</pre>
  <p>Chaque liste (<code>INDEX</code>) est signée (<code>INDEX.minisig</code>, minisign) et
  chaque paquet a son empreinte SHA-256 : npk refuse tout fichier modifié.</p>
</div>
<table>
<tr><th>Paquet</th><th>Version</th><th>Taille</th><th>Description</th></tr>
EOF
while IFS='|' read -r name ver deps sha file desc; do
    size=$(ls -l "$REPO/$file" | awk '{ s=$5; if (s >= 1048576) printf "%.1f Mo", s/1048576; else printf "%d Ko", (s+1023)/1024 }')
    printf '<tr><td><a href="%s">%s</a></td><td class="v">%s</td><td class="s">%s</td><td>%s</td></tr>\n' \
        "$file" "$(printf %s "$name" | html_escape)" "$(printf %s "$ver" | html_escape)" "$size" \
        "$(printf %s "$desc" | html_escape)"
done < "$REPO/INDEX"
cat <<EOF
</table>
</main></body>
</html>
EOF
} > "$OUT/index.html"

# ---- Branche gh-pages : un seul commit, poussé en remplaçant l'ancien ----
NAME=$(git -C "$ROOT" config user.name || echo "Nexus OS")
MAIL=$(git -C "$ROOT" config user.email || echo "nexus@localhost")
cd "$OUT"
git init -q -b main
git config user.name "$NAME"
git config user.email "$MAIL"
git add -A
git commit -q -m "Dépôt de paquets Nexus OS $NEXUS_VERSION ($NB paquets)"
if [ "${DRY_RUN:-0}" = 1 ]; then echo "[nexus] Essai : site prêt dans .pages, rien n'a été envoyé"; exit 0; fi
echo "[nexus] Envoi vers $REMOTE (branche main)"
GIT_TERMINAL_PROMPT=0 git push -q --force "$REMOTE" main
echo "[nexus] Publié. Adresse (une fois GitHub Pages activé sur la branche main) :"
echo "        $(echo "$REMOTE" | sed -E 's#https://github.com/([^/]+)/([^/.]+)(\.git)?#https://\1.github.io/\2/#')"
