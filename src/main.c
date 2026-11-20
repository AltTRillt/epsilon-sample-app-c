// ============================================================================
//  SANS - combat style Undertale pour NumWorks (application native .nwa)
//  Port en C de la version Python. Attaques : lasers, carre, cercles, refuge,
//  diagonales, mur, damier, pluie de traits. Parry (OK), phases, mode test (3).
//
//  - Tous les temps sont en millisecondes (entiers) : pas de flottants.
//  - Aucune allocation dynamique, aucune fonction de la libc : tout est statique.
//  - Les touches sont lues avec eadk_keyboard_scan() (un champ de bits).
// ============================================================================
#include <eadk.h>
#include <stdint.h>
#include <stdbool.h>

// ----------------------------------------------------------------------------
//  Reglages (les memes que dans la version Python)
// ----------------------------------------------------------------------------
#define DUREE 90            // duree du combat en secondes
#define AVERT 750           // temps d'avertissement avant les attaques (ms)
#define TIR 350             // duree d'un laser (ms)
#define VITESSE 150         // vitesse du joueur (pixels/seconde)
#define GROW 15             // croissance de la boite a l'explosion (pixels)
#define CERCLES_MIN 5       // nombre de cercles au debut
#define CERCLES_MAX 20      // nombre de cercles a la fin (augmente avec le temps)
#define BOITE 75            // cote du carre (pixels)
#define RAYON 15            // rayon des cercles
#define GROWC 5             // croissance des cercles a l'explosion (pixels)
#define PARRY_DUREE 1000    // duree pendant laquelle le parry est actif apres OK (ms)
#define PARRY_CD 150        // delai avant de pouvoir re-parry (ms)
#define PARRY_TOL 2         // tolerance (pixels) pour etre "pile" au croisement des lasers
#define DIAG_T 750          // duree des lasers en diagonale : parry avant la fin sinon mort (ms)
#define DIAG_REFRESH 50     // les diagonales suivent le joueur toutes les 50 ms

#define MUR_FOIS 2          // le mur est toujours fait 2 fois d'affilee
#define MUR_NB 12           // nombre de petits traits dans le mur
#define MUR_BARRE 2         // epaisseur d'un trait (pixels)
#define MUR_ECART 10        // distance entre deux traits (pixels)
#define MUR_L ((MUR_NB - 1) * MUR_ECART + MUR_BARRE)   // longueur totale du groupe de traits
#define MUR_TROU 55         // hauteur du trou dans les traits (pixels)
#define MUR_V 750           // vitesse du mur (pixels/seconde)

#define DAM_COLS 8          // damier : colonnes (cases de 40 px de large)
#define DAM_ROWS 6          // damier : lignes (cases de 37 px de haut)
#define DAM_VAGUES 2        // nombre de vagues (la 1ere sur la case du joueur, puis l'inverse)

#define PLUIE_N 14          // pluie : nombre de traits
#define PLUIE_L 4           // largeur d'un trait (pixels)
#define PLUIE_H 50          // hauteur d'un trait (pixels)
#define PLUIE_V 260         // vitesse de chute (pixels/seconde)
#define PLUIE_ETALE 1600    // les traits partent a des moments differents sur cette duree (ms)

#define REFUGE_R 30         // rayon du cercle refuge (ou il faut aller)
#define REFUGE_SHRINK 1500  // duree pendant laquelle l'arene se remplit (ms)
#define REFUGE_STEPS 6      // nombre d'etapes de remplissage
#define REFUGE_HOLD 500     // duree pendant laquelle tout reste rempli (ms)

#define ZONE_T 5            // secondes max entre bordure blanche et rouge
#define IDLE_T 10           // secondes max sans bouger
#define IDLE_ALERTE 5       // le compte a rebours apparait quand il reste 5 s

// Phases : (90->70), (70->30), (30->fin). Seuils en secondes de temps restant.
static const int PAL_S[3] = {70, 30, 0};
static const int PAL_C[3] = {1500, 1000, 850};   // cadence des attaques (ms)
static const int REPET[3] = {1, 2, 3};           // repetitions de chaque attaque par phase
#define NPAL 3

// ----------------------------------------------------------------------------
//  Ecran, couleurs (RGB565) et touches
// ----------------------------------------------------------------------------
#define LW 320
#define LH 222
#define EP 3                // epaisseur de la bordure rouge (mort instantanee)
#define MB 18               // distance de la bordure blanche au bord de l'ecran
#define IN (MB + 2)         // limite de la zone sure (interieur de la bordure blanche)
#define T 8                 // taille du joueur

#define C_N 0x0000          // noir
#define C_B 0xFFFF          // blanc
#define C_R 0xF800          // rouge
#define C_P 0x07FF          // cyan (joueur pendant le parry)

// numeros de touches (bits de eadk_keyboard_scan)
#define K_LEFT 0
#define K_UP 1
#define K_DOWN 2
#define K_RIGHT 3
#define K_OK 4
#define K_BACK 5
#define K_HOME 6
#define K_THREE 44
#define K_EXE 52

#define INF 2000000000

static uint32_t t_base;     // heure de depart (ms)
static uint32_t ks_lo, ks_hi;

static int now_ms(void) { return (int)((uint32_t)eadk_timing_millis() - t_base); }

static void scan(void) {
  uint64_t s = eadk_keyboard_scan();
  ks_lo = (uint32_t)s;
  ks_hi = (uint32_t)(s >> 32);
}

static int key(int b) {
  if (b < 32) return (int)((ks_lo >> b) & 1u);
  return (int)((ks_hi >> (b - 32)) & 1u);
}

static int mini(int a, int b) { return a < b ? a : b; }
static int maxi(int a, int b) { return a > b ? a : b; }
static int absi(int a) { return a < 0 ? -a : a; }

static uint32_t rng_state = 2463534242u;
static void rng_seed(uint32_t s) { rng_state = s ? s : 2463534242u; }
static int rnd(int a, int b) {          // entier au hasard entre a et b inclus
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return a + (int)(rng_state % (uint32_t)(b - a + 1));
}

static int isqrt(int n) { int r = 0; while ((r + 1) * (r + 1) <= n) r++; return r; }

// ----------------------------------------------------------------------------
//  Dessin
// ----------------------------------------------------------------------------
// Rectangle plein, decoupe aux bords de l'ecran (ignore les rectangles vides)
static void rect(int x, int y, int w, int h, int c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LW) w = LW - x;
  if (y + h > LH) h = LH - y;
  if (w <= 0 || h <= 0) return;
  eadk_display_push_rect_uniform(
      (eadk_rect_t){(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h}, (uint16_t)c);
}

static void txt(const char *s, int x, int y, int fg, int bg) {
  eadk_display_draw_string(s, (eadk_point_t){(uint16_t)x, (uint16_t)y}, true,
                           (uint16_t)fg, (uint16_t)bg);
}

// (les __asm__ vides empechent GCC de transformer ces boucles en appels strlen/memcpy de la libc)
static int slen(const char *s) { int n = 0; while (s[n]) { n++; __asm__ volatile("" ::: "memory"); } return n; }
static void scat(char *d, const char *s) {       // d += s
  int n = slen(d), i = 0;
  while (s[i]) { d[n + i] = s[i]; i++; __asm__ volatile("" ::: "memory"); }
  d[n + i] = 0;
}
static void itos(int v, char *d) {                // entier positif -> texte
  char tmp[12]; int n = 0, i;
  if (v <= 0) { d[0] = '0'; d[1] = 0; return; }
  while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
  for (i = 0; i < n; i++) d[i] = tmp[n - 1 - i];
  d[n] = 0;
}

static int inter(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
  return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

// Tables des demi-largeurs des cercles (rayons 15, 20 et 30)
static int tab15[2 * RAYON + 1];
static int tab20[2 * (RAYON + GROWC) + 1];
static int tab30[2 * REFUGE_R + 1];
static void init_tables(void) {
  int d;
  for (d = -RAYON; d <= RAYON; d++) tab15[d + RAYON] = isqrt(RAYON * RAYON - d * d);
  for (d = -(RAYON + GROWC); d <= RAYON + GROWC; d++)
    tab20[d + RAYON + GROWC] = isqrt((RAYON + GROWC) * (RAYON + GROWC) - d * d);
  for (d = -REFUGE_R; d <= REFUGE_R; d++) tab30[d + REFUGE_R] = isqrt(REFUGE_R * REFUGE_R - d * d);
}
static const int *tabptr(int r) {
  if (r == RAYON) return tab15;
  if (r == RAYON + GROWC) return tab20;
  return tab30;
}

// ----------------------------------------------------------------------------
//  Les attaques : un tableau d'entiers par attaque (memes indices que le Python)
//   X,Y,W,H   : rectangle (pour les cercles : boite englobante)
//   TT, TF    : instant du tir, instant de fin (ms)
//   ET        : 0 = avertissement, 1 = tir
//   CT        : 1 = contour pendant l'avertissement (boite, cercle, case)
//   WX..WH    : trait d'avertissement (lasers)
//   TY        : 0 = laser/boite, 1 = cercle, 2 = case du damier
//   P13,P14   : laser = coin du trou ; cercle = centre
//   P15       : rayon du cercle
//   P16       : laser : parry reussi
//   P17       : 1 = cercle du refuge
// ----------------------------------------------------------------------------
enum { X, Y, W, H, TT, TF, ET, CT, WX, WY, WW, WH, TY, P13, P14, P15, P16, P17, NF };
#define MAXA 128
static int atts[MAXA][NF];
static int natt;

static int *new_att(void) {
  int *a;
  if (natt >= MAXA) return 0;
  a = atts[natt++];
  a[X] = 0; a[Y] = 0; a[W] = 0; a[H] = 0; a[TT] = 0; a[TF] = 0; a[ET] = 0; a[CT] = 0;
  a[WX] = 0; a[WY] = 0; a[WW] = 0; a[WH] = 0; a[TY] = 0; a[P13] = 0; a[P14] = 0;
  a[P15] = 0; a[P16] = 0; a[P17] = 0;
  return a;
}

static void copy_att(int *d, const int *s) {
  d[X] = s[X]; d[Y] = s[Y]; d[W] = s[W]; d[H] = s[H]; d[TT] = s[TT]; d[TF] = s[TF];
  d[ET] = s[ET]; d[CT] = s[CT]; d[WX] = s[WX]; d[WY] = s[WY]; d[WW] = s[WW]; d[WH] = s[WH];
  d[TY] = s[TY]; d[P13] = s[P13]; d[P14] = s[P14]; d[P15] = s[P15]; d[P16] = s[P16];
  d[P17] = s[P17];
}

static void remove_att(int i) {       // on remplace par la derniere (l'ordre n'a pas d'importance)
  natt--;
  if (i != natt) copy_att(atts[i], atts[natt]);
}

static void cercle(const int *a) {
  int cx = a[P13], cy = a[P14], r = a[P15], i;
  const int *t = tabptr(r);
  for (i = 0; i < 2 * r + 1; i++) {
    int hw = t[i], y = cy - r + i, d = i - r;
    if (a[ET] == 1 || absi(d) >= r - 2) {
      rect(cx - hw, y, 2 * hw + 1, 1, C_B);
    } else {
      int hi = isqrt((r - 2) * (r - 2) - d * d);
      int w = maxi(1, hw - hi);
      rect(cx - hw, y, w, 1, C_B);
      rect(cx + hw - w + 1, y, w, 1, C_B);
    }
  }
}

static void dessiner(const int *a) {
  int x = a[X], y = a[Y], w = a[W], h = a[H];
  if (a[TY] == 1) { cercle(a); return; }
  if (a[ET] == 1) {
    rect(x, y, w, h, C_B);
    if (!a[CT]) rect(a[P13], a[P14], T, T, C_N);      // trou du laser
  } else if (a[CT]) {
    rect(x, y, w, 2, C_B);
    rect(x, y + h - 2, w, 2, C_B);
    rect(x, y, 2, h, C_B);
    rect(x + w - 2, y, 2, h, C_B);
  } else {
    rect(a[WX], a[WY], a[WW], a[WH], C_B);
  }
}

// Bordures : rouge (mort instantanee) puis blanche (zone sure a l'interieur)
static const int BD[8][5] = {
  {0, 0, LW, EP, C_R}, {0, LH - EP, LW, EP, C_R},
  {0, EP, EP, LH - 2 * EP, C_R}, {LW - EP, EP, EP, LH - 2 * EP, C_R},
  {MB, MB, LW - 2 * MB, 2, C_B}, {MB, LH - MB - 2, LW - 2 * MB, 2, C_B},
  {MB, MB + 2, 2, LH - 2 * MB - 4, C_B}, {LW - MB - 2, MB + 2, 2, LH - 2 * MB - 4, C_B}};

static void bordures(int c0, int c1) {     // seulement les colonnes c0..c1
  int i;
  for (i = 0; i < 8; i++) {
    int x0 = maxi(BD[i][0], c0), x1 = mini(BD[i][0] + BD[i][2], c1);
    if (x1 > x0) rect(x0, BD[i][1], x1 - x0, BD[i][3], BD[i][4]);
  }
}

static int a_toucher(const int *a, int x, int y, int w, int h) {
  // faut-il redessiner l'attaque a apres avoir efface le rectangle ?
  if (!inter(x, y, w, h, a[X], a[Y], a[W], a[H])) return 0;
  if (a[TY] == 1 && a[ET] == 0) {
    int far = 0, i, j;
    int qxs[2], qys[2];
    qxs[0] = x; qxs[1] = x + w - 1; qys[0] = y; qys[1] = y + h - 1;
    for (i = 0; i < 2; i++)
      for (j = 0; j < 2; j++) {
        int d = (qxs[i] - a[P13]) * (qxs[i] - a[P13]) + (qys[j] - a[P14]) * (qys[j] - a[P14]);
        if (d > far) far = d;
      }
    return far > (a[P15] - 2) * (a[P15] - 2);
  }
  return 1;
}

static int dans_cercle(int cx, int cy, int r, int nx, int ny) {
  int i, j;
  int qxs[2], qys[2];
  qxs[0] = nx; qxs[1] = nx + T - 1; qys[0] = ny; qys[1] = ny + T - 1;
  for (i = 0; i < 2; i++)
    for (j = 0; j < 2; j++)
      if ((qxs[i] - cx) * (qxs[i] - cx) + (qys[j] - cy) * (qys[j] - cy) > r * r) return 0;
  return 1;
}

// Deux diagonales passant par (cx, cy), faites de petits blocs 4x4 tous les 3 px.
// clip : si non nul, on ne dessine que ce qui touche le rectangle (x, y, w, h).
static void diag_dessiner(int cx, int cy, int c, int clip, int x, int y, int w, int h) {
  int s;
  for (s = 1; s >= -1; s -= 2) {
    int kd, kf, lo, hi, k;
    if (s == 1) { kd = -mini(cx, cy); kf = mini(LW - 1 - cx, LH - 1 - cy); }
    else { kd = -mini(cx, LH - 1 - cy); kf = mini(LW - 1 - cx, cy); }
    lo = kd; hi = kf;
    if (clip) {
      lo = maxi(lo, x - cx - 1);
      hi = mini(hi, x + w - cx + 1);
      if (s == 1) { lo = maxi(lo, y - cy - 1); hi = mini(hi, y + h - cy + 1); }
      else { lo = maxi(lo, cy - y - h - 1); hi = mini(hi, cy - y + 1); }
    }
    k = kd + ((lo - kd + 2) / 3) * 3;
    while (k <= hi) {
      rect(cx + k - 2, cy + s * k - 2, 4, 4, c);
      k += 3;
    }
  }
}

// Valeurs exposees pour les tests (ne servent pas au jeu)
static int dbg_px, dbg_py, dbg_hy, dbg_rcx, dbg_rcy;

static int g_etat = 1;                 // 0 pendant le refuge/le mur : on ne redessine pas les bordures
static int g_diag_on = 0, g_dcx = 0, g_dcy = 0;

// Efface un rectangle puis redessine ce qui le touche (attaques, diagonales, bordures)
static void restaurer(int x, int y, int w, int h) {
  int i;
  rect(x, y, w, h, C_N);
  for (i = 0; i < natt; i++)
    if (a_toucher(atts[i], x, y, w, h)) dessiner(atts[i]);
  if (g_diag_on) diag_dessiner(g_dcx, g_dcy, C_B, 1, x, y, w, h);
  if (g_etat && !(x >= IN && y >= IN && x + w <= LW - IN && y + h <= LH - IN)) bordures(0, LW);
}

// ----------------------------------------------------------------------------
//  Creation des attaques
// ----------------------------------------------------------------------------
static void laser_v(int cx, int cy, int t) {
  int *a = new_att();
  if (!a) return;
  a[X] = maxi(0, mini(LW - 12, cx - 6)); a[Y] = 0; a[W] = 12; a[H] = LH;
  a[TT] = t + AVERT; a[TF] = t + AVERT + TIR; a[ET] = 0; a[CT] = 0;
  a[WX] = cx - 1; a[WY] = 0; a[WW] = 2; a[WH] = LH; a[TY] = 0;
  a[P13] = cx - 4; a[P14] = cy - 4;
  dessiner(a);
}

static void laser_h(int cx, int cy, int t) {
  int *a = new_att();
  if (!a) return;
  a[X] = 0; a[Y] = maxi(0, mini(LH - 12, cy - 6)); a[W] = LW; a[H] = 12;
  a[TT] = t + AVERT; a[TF] = t + AVERT + TIR; a[ET] = 0; a[CT] = 0;
  a[WX] = 0; a[WY] = cy - 1; a[WW] = LW; a[WH] = 2; a[TY] = 0;
  a[P13] = cx - 4; a[P14] = cy - 4;
  dessiner(a);
}

static void boite(int t, int px, int py) {
  int *a = new_att();
  if (!a) return;
  a[X] = maxi(GROW, mini(LW - BOITE - GROW, px - BOITE / 2));
  a[Y] = maxi(GROW, mini(LH - BOITE - GROW, py - BOITE / 2));
  a[W] = BOITE; a[H] = BOITE;
  a[TT] = t + AVERT; a[TF] = t + AVERT + 400; a[ET] = 0; a[CT] = 1; a[TY] = 0;
  dessiner(a);
}

static void cellule(int t, int x, int y, int w, int h) {
  int *a = new_att();
  if (!a) return;
  a[X] = x; a[Y] = y; a[W] = w; a[H] = h;
  a[TT] = t + AVERT; a[TF] = t + AVERT + 400; a[ET] = 0; a[CT] = 1; a[TY] = 2;
  dessiner(a);
}

// Un cercle qui ne chevauche aucun autre cercle deja la (60 essais)
static void rond(int t) {
  int r2 = RAYON + GROWC, essai, i;
  for (essai = 0; essai < 60; essai++) {
    int cx = rnd(r2, LW - r2 - 1), cy = rnd(r2, LH - r2 - 1), ok = 1;
    for (i = 0; i < natt; i++)
      if (atts[i][TY] == 1) {
        int dx = cx - atts[i][P13], dy = cy - atts[i][P14];
        if (dx * dx + dy * dy < (2 * r2 + 2) * (2 * r2 + 2)) { ok = 0; break; }
      }
    if (ok) {
      int *a = new_att();
      if (!a) return;
      a[X] = cx - r2; a[Y] = cy - r2; a[W] = 2 * r2 + 1; a[H] = 2 * r2 + 1;
      a[TT] = t + AVERT; a[TF] = t + AVERT + 400; a[ET] = 0; a[CT] = 1; a[TY] = 1;
      a[P13] = cx; a[P14] = cy; a[P15] = RAYON;
      dessiner(a);
      return;
    }
  }
}

// ----------------------------------------------------------------------------
//  Phases et suite des attaques
// ----------------------------------------------------------------------------
static int phase(int reste_ms) {         // 0, 1 ou 2 selon le temps restant
  int i;
  for (i = 0; i < NPAL; i++)
    if (reste_ms > PAL_S[i] * 1000) return i;
  return NPAL - 1;
}

static int cadence(int reste_ms) {
  int i;
  for (i = 0; i < NPAL; i++)
    if (reste_ms > PAL_S[i] * 1000) return PAL_C[i];
  return PAL_C[NPAL - 1];
}

// 0 lasers, 1 boite, 2 cercles, 3 refuge, 4 diagonales, 5 mur, 6 damier, 7 pluie
#define NATT 8
static int suite[NATT * 3 + 4];
static int suite_n, suite_i;

static void construire(int ph) {
  int ts[NATT], i, j, t, n, k, tmp;
  for (i = 0; i < NATT; i++) ts[i] = i;
  for (i = NATT - 1; i > 0; i--) {         // melange (Fisher-Yates)
    j = rnd(0, i);
    tmp = ts[i]; ts[i] = ts[j]; ts[j] = tmp;
  }
  suite_n = 0; suite_i = 0;
  for (i = 0; i < NATT; i++) {
    t = ts[i];
    n = REPET[ph];
    if (t == 5) n = MUR_FOIS;
    for (k = 0; k < n; k++) suite[suite_n++] = t;
  }
}

// ----------------------------------------------------------------------------
//  Le mur : traits verticaux avec un trou, qui traversent l'ecran en horizontal
// ----------------------------------------------------------------------------
static void mur_barre(int x, int hy, int c) {
  int x0 = maxi(0, x), x1 = mini(LW, x + MUR_BARRE);
  if (x1 > x0) {
    rect(x0, 0, x1 - x0, hy, c);
    rect(x0, hy + MUR_TROU, x1 - x0, LH - hy - MUR_TROU, c);
  }
}

// Fait avancer tous les traits de xa a xb : on efface ce qu'ils laissent derriere eux,
// on redessine les bordures, puis on dessine les traits plus loin.
static void mur_pas(int xa, int xb, int hy) {
  int lo = LW, hi = 0, i;
  for (i = 0; i < MUR_NB; i++) {
    int a = xa + i * MUR_ECART, b = xb + i * MUR_ECART, e0, e1;
    if (b > a) { e0 = a; e1 = mini(a + MUR_BARRE, b); }
    else { e0 = maxi(a, b + MUR_BARRE); e1 = a + MUR_BARRE; }
    e0 = maxi(0, e0);
    e1 = mini(LW, e1);
    if (e1 > e0) {
      rect(e0, 0, e1 - e0, LH, C_N);
      lo = mini(lo, e0);
      hi = maxi(hi, e1);
    }
  }
  if (hi > lo) bordures(lo, hi);
  for (i = 0; i < MUR_NB; i++) mur_barre(xb + i * MUR_ECART, hy, C_B);
}

// ----------------------------------------------------------------------------
//  La partie
// ----------------------------------------------------------------------------
enum { R_MENU, R_V, R_BORD, R_ZONE, R_IDLE, R_ATT, R_PARRY, R_FILL, R_MUR, R_TRAIT };
static const char *const MSG[] = {
  "", "V", "Bordure rouge", "Trop longtemps hors zone", "Trop immobile",
  "Touche par une attaque", "Parry rate", "Touche le remplissage", "Touche le mur",
  "Touche un trait"};

// test < 0 : partie normale ; sinon numero de l'attaque a tester (mode test)
static int jeu(int test, int ph_test) {
  int px = 156000, py = 107000;          // position en millipixels
  int debut, decal = 0, t0, last, na, ancien = -1, dernier_move;
  int zone_on = 0, t_zone = 0;
  int lab_on = 0, lab_x = 0, lab_y = 0, lab_n = 0;
  int suite_ph = -1;
  // etats : 0 = rien, 1 = attente (ecran pas encore vide), 2 = actif
  int damier_etat = 0, dam_v = 0, dam_p = 0;
  int pluie_etat = 0, nbar = 0, pluie_t0 = 0;
  int bar_x[PLUIE_N], bar_y[PLUIE_N], bar_t[PLUIE_N];
  int mur_etat = 0, mur_sens, mur_hy = 0, mur_x = 0, mur_x0 = 0, mur_ex = 0, mur_t0 = 0, mur_lance = 0;
  int parry_fin = 0, parry_libre = 0, ok_prec = 1;
  int refuge_etat = 0, rcx = 0, rcy = 0, rt0 = 0, rk = 0, rfin = 0;
  int ro0 = 0, ro1 = 0, ro2 = LW, ro3 = LH;
  int diag_etat = 0, d_t0 = 0, d_ref = 0;
  int i;

  rect(0, 0, LW, LH, C_N);
  bordures(0, LW);
  natt = 0;
  g_etat = 1; g_diag_on = 0;
  debut = now_ms();
  if (test >= 0 && ph_test > 0) decal = (DUREE - PAL_S[ph_test - 1]) * 1000;  // debut de la phase choisie
  t0 = debut - decal;
  last = debut;
  na = debut + 1000;
  dernier_move = debut;
  mur_sens = (rnd(0, 1) == 0) ? 1 : -1;
  suite_n = 0; suite_i = 0;

  for (;;) {
    int now = now_ms();
    int dt = now - last;
    int ox, oy, nx, ny, v, ok, parry, rz_on = 0, rz = 0, ri, c, reste, n_aff;
    last = now;
    ox = px / 1000; oy = py / 1000;
    scan();
    v = VITESSE * dt;
    if (key(K_LEFT)) px -= v;
    if (key(K_RIGHT)) px += v;
    if (key(K_UP)) py -= v;
    if (key(K_DOWN)) py += v;
    px = maxi(0, mini((LW - T) * 1000, px));
    py = maxi(0, mini((LH - T) * 1000, py));
    if (key(K_BACK) || (test >= 0 && key(K_THREE))) return R_MENU;   // quitter
    ok = key(K_OK);
    if (ok && !ok_prec && now >= parry_libre) {
      parry_fin = now + PARRY_DUREE;
      parry_libre = now + PARRY_DUREE + PARRY_CD;
    }
    ok_prec = ok;
    parry = now < parry_fin;
    nx = px / 1000; ny = py / 1000;
    dbg_px = nx; dbg_py = ny;

    if (now - t0 >= DUREE * 1000) return R_V;
    if (nx != ox || ny != oy) dernier_move = now;

    // bordure rouge = mort instantanee
    if (nx < EP || ny < EP || nx + T > LW - EP || ny + T > LH - EP) return R_BORD;

    // entre la bordure blanche et la rouge : 5 s max
    if (nx < IN || ny < IN || nx + T > LW - IN || ny + T > LH - IN) {
      if (!zone_on) { zone_on = 1; t_zone = now; }
      rz = ZONE_T * 1000 - (now - t_zone);
      rz_on = 1;
      if (rz <= 0) return R_ZONE;
    } else {
      zone_on = 0;
    }

    // immobile trop longtemps
    ri = IDLE_T * 1000 - (now - dernier_move);
    if (ri <= 0) return R_IDLE;

    // nouvelle attaque : on suit la suite de la phase en cours
    if (now >= na && !(diag_etat || refuge_etat || mur_etat || damier_etat || pluie_etat)) {
      if (test >= 0) {
        c = test;                          // mode test : toujours la meme attaque
      } else {
        int ph = phase(DUREE * 1000 - (now - t0));
        if (suite_i >= suite_n || ph != suite_ph) { construire(ph); suite_ph = ph; }
        c = suite[suite_i++];
      }
      if (c == 0) { laser_v(nx + 4, ny + 4, now); laser_h(nx + 4, ny + 4, now); }
      else if (c == 3) refuge_etat = 1;
      else if (c == 4) diag_etat = 1;
      else if (c == 5) mur_etat = 1;
      else if (c == 6) damier_etat = 1;
      else if (c == 7) pluie_etat = 1;
      else if (c == 1) boite(now, nx + 4, ny + 4);
      else {
        int nbc = mini(CERCLES_MAX, CERCLES_MIN + ((CERCLES_MAX - CERCLES_MIN + 1) * (now - t0)) / (DUREE * 1000));
        int k;
        for (k = 0; k < nbc; k++) rond(now);
      }
      na = now + cadence(DUREE * 1000 - (now - t0));
    }

    // mise a jour des attaques
    for (i = 0; i < natt;) {
      int *a = atts[i];
      if (a[ET] == 0 && now >= a[TT]) {
        a[ET] = 1;
        if (a[TY] == 1) {
          a[P15] += GROWC;
        } else if (a[TY] == 0 && a[CT]) {       // la boite grandit
          int x2 = mini(LW, a[X] + a[W] + GROW), y2 = mini(LH, a[Y] + a[H] + GROW);
          a[X] = maxi(0, a[X] - GROW);
          a[Y] = maxi(0, a[Y] - GROW);
          a[W] = x2 - a[X];
          a[H] = y2 - a[Y];
        }
        dessiner(a);
        i++;
      } else if (a[ET] == 1 && now >= a[TF]) {
        int ax = a[X], ay = a[Y], aw = a[W], ah = a[H], j;
        rect(ax, ay, aw, ah, C_N);
        remove_att(i);                           // l'attaque echangee sera traitee au tour suivant
        for (j = 0; j < natt; j++)
          if (a_toucher(atts[j], ax, ay, aw, ah)) dessiner(atts[j]);
        bordures(0, LW);
        ancien = -1;
      } else {
        i++;
      }
    }

    // lasers en diagonale : suivent le joueur jusqu'au parry
    if (diag_etat == 1 && natt == 0) {
      diag_etat = 2;
      g_dcx = nx + 4; g_dcy = ny + 4;
      d_t0 = now; d_ref = now;
      diag_dessiner(g_dcx, g_dcy, C_B, 0, 0, 0, 0, 0);
    } else if (diag_etat == 2) {
      if (parry && parry_fin - PARRY_DUREE >= d_t0) {
        diag_dessiner(g_dcx, g_dcy, C_N, 0, 0, 0, 0, 0);     // parry reussi
        bordures(0, LW);
        diag_etat = 0;
        na = now + cadence(DUREE * 1000 - (now - t0));
        ancien = -1;
      } else if (now - d_t0 >= DIAG_T) {
        return R_PARRY;
      } else if (now - d_ref >= DIAG_REFRESH) {
        diag_dessiner(g_dcx, g_dcy, C_N, 0, 0, 0, 0, 0);
        bordures(0, LW);
        g_dcx = nx + 4; g_dcy = ny + 4;
        diag_dessiner(g_dcx, g_dcy, C_B, 0, 0, 0, 0, 0);
        d_ref = now;
        ancien = -1;
      }
    }
    g_diag_on = (diag_etat == 2);

    // refuge : aller dans le cercle avant que l'arene se remplisse
    if (refuge_etat == 1 && natt == 0) {
      int bd = -1, bqx = 0, bqy = 0, k;
      int *a;
      for (k = 0; k < 5; k++) {                   // on garde la position la plus eloignee
        int qx = rnd(REFUGE_R + 4, LW - REFUGE_R - 5), qy = rnd(REFUGE_R + 4, LH - REFUGE_R - 5);
        int d = (qx - nx - 4) * (qx - nx - 4) + (qy - ny - 4) * (qy - ny - 4);
        if (bd < 0 || d > bd) { bd = d; bqx = qx; bqy = qy; }
      }
      rcx = bqx; rcy = bqy; rt0 = now; rk = 0; rfin = 0;
      dbg_rcx = rcx; dbg_rcy = rcy;
      ro0 = 0; ro1 = 0; ro2 = LW; ro3 = LH;
      a = new_att();
      if (a) {
        a[X] = 0; a[Y] = 0; a[W] = LW; a[H] = LH; a[TT] = INF; a[TF] = INF; a[ET] = 0; a[CT] = 1;
        a[TY] = 1; a[P13] = rcx; a[P14] = rcy; a[P15] = REFUGE_R; a[P17] = 1;
        dessiner(a);
      }
      g_etat = 0;
      refuge_etat = 2;
    } else if (refuge_etat == 2) {
      int tr = now - rt0;
      if (tr >= AVERT) {
        int cible = mini(REFUGE_STEPS, ((tr - AVERT) * REFUGE_STEPS) / REFUGE_SHRINK + 1);
        while (rk < cible) {
          int bx0, by0, bx1, by1, x0, y0, x1, y1;
          rk++;
          bx0 = rcx - REFUGE_R; by0 = rcy - REFUGE_R;
          bx1 = rcx + REFUGE_R + 1; by1 = rcy + REFUGE_R + 1;
          x0 = bx0 * rk / REFUGE_STEPS; y0 = by0 * rk / REFUGE_STEPS;
          x1 = LW - (LW - bx1) * rk / REFUGE_STEPS; y1 = LH - (LH - by1) * rk / REFUGE_STEPS;
          rect(ro0, ro1, ro2 - ro0, y0 - ro1, C_B);
          rect(ro0, y1, ro2 - ro0, ro3 - y1, C_B);
          rect(ro0, y0, x0 - ro0, y1 - y0, C_B);
          rect(x1, y0, ro2 - x1, y1 - y0, C_B);
          ro0 = x0; ro1 = y0; ro2 = x1; ro3 = y1;
          ancien = -1;
        }
        if (rk >= REFUGE_STEPS && !rfin) {        // derniere etape : on remplit les coins
          const int *tb = tab30;
          for (i = 0; i < 2 * REFUGE_R + 1; i++) {
            int hw = tb[i], w = REFUGE_R - hw;
            rect(rcx - REFUGE_R, rcy - REFUGE_R + i, w, 1, C_B);
            rect(rcx + hw + 1, rcy - REFUGE_R + i, w, 1, C_B);
          }
          rfin = 1;
        }
        // toucher le remplissage = mort
        if (rfin) {
          if (!dans_cercle(rcx, rcy, REFUGE_R - 2, nx, ny)) return R_FILL;
        } else if (rk > 0) {
          if (nx < ro0 || ny < ro1 || nx + T > ro2 || ny + T > ro3) return R_FILL;
        }
        if (tr >= AVERT + REFUGE_SHRINK + REFUGE_HOLD) {   // fin : on efface tout
          for (i = 0; i < natt; i++)
            if (atts[i][P17] == 1) { remove_att(i); break; }
          rect(0, 0, LW, LH, C_N);
          g_etat = 1;
          bordures(0, LW);
          refuge_etat = 0;
          lab_on = 0;
          na = now + cadence(DUREE * 1000 - (now - t0));
          ancien = -1;
        }
      }
    }

    // mur avec un trou : traverse l'ecran en horizontal, sens alterne
    if (mur_etat == 1 && natt == 0) {
      mur_sens = -mur_sens;
      mur_hy = rnd(8, LH - MUR_TROU - 8);
      dbg_hy = mur_hy;
      mur_t0 = now;
      mur_lance = 0;
      if (mur_sens > 0) { mur_x0 = -MUR_L; mur_ex = 0; }
      else { mur_x0 = LW; mur_ex = LW - 5; }
      mur_x = mur_x0;
      // avertissement : le couloir (trou) + un marqueur du cote d'entree
      rect(0, mur_hy - 2, LW, 2, C_B);
      rect(0, mur_hy + MUR_TROU, LW, 2, C_B);
      rect(mur_ex, 0, 5, mur_hy, C_B);
      rect(mur_ex, mur_hy + MUR_TROU, 5, LH - mur_hy - MUR_TROU, C_B);
      g_etat = 0;
      mur_etat = 2;
    } else if (mur_etat == 2) {
      int tr = now - mur_t0;
      if (tr >= AVERT) {
        int xb;
        if (!mur_lance) {
          mur_lance = 1;
          rect(0, mur_hy - 2, LW, 2, C_N);
          rect(0, mur_hy + MUR_TROU, LW, 2, C_N);
          rect(mur_ex, 0, 5, LH, C_N);
          bordures(0, LW);
        }
        xb = mur_x0 + mur_sens * (MUR_V * (tr - AVERT) / 1000);
        mur_pas(mur_x, xb, mur_hy);
        mur_x = xb;
        if (nx < mur_x + MUR_L && nx + T > mur_x) {
          for (i = 0; i < MUR_NB; i++) {
            int bx = mur_x + i * MUR_ECART;
            if (inter(nx, ny, T, T, bx, 0, MUR_BARRE, mur_hy) ||
                inter(nx, ny, T, T, bx, mur_hy + MUR_TROU, MUR_BARRE, LH - mur_hy - MUR_TROU))
              return R_MUR;
          }
        }
        if ((mur_sens > 0 && mur_x >= LW) || (mur_sens < 0 && mur_x + MUR_L <= 0)) {
          g_etat = 1;                             // le mur est sorti : fin de l'attaque
          bordures(0, LW);
          mur_etat = 0;
          na = now + cadence(DUREE * 1000 - (now - t0));
          ancien = -1;
        }
      }
    }

    // damier : une moitie des cases explose (celle du joueur d'abord), puis l'autre
    if (damier_etat == 1 && natt == 0) {
      dam_p = ((nx + 4) / (LW / DAM_COLS) + (ny + 4) / (LH / DAM_ROWS)) % 2;
      dam_v = 0;
      damier_etat = 2;
    }
    if (damier_etat == 2 && natt == 0) {
      if (dam_v >= DAM_VAGUES) {
        damier_etat = 0;
        na = now + cadence(DUREE * 1000 - (now - t0));
      } else {
        int cw = LW / DAM_COLS, ch = LH / DAM_ROWS, par = (dam_p + dam_v) % 2, rr, cc;
        for (rr = 0; rr < DAM_ROWS; rr++)
          for (cc = 0; cc < DAM_COLS; cc++)
            if ((rr + cc) % 2 == par) cellule(now, cc * cw, rr * ch, cw, ch);
        dam_v++;
      }
    }

    // pluie de traits : ils tombent du haut, certains visent la colonne du joueur
    if (pluie_etat == 1 && natt == 0) {
      int k;
      pluie_t0 = now;
      nbar = 0;
      for (k = 0; k < PLUIE_N; k++) {
        int bx;
        if (k % 3 == 0) bx = maxi(4, mini(LW - 4 - PLUIE_L, nx + 4 + rnd(-14, 14)));   // vise le joueur
        else bx = rnd(4, LW - 4 - PLUIE_L);                                              // colonne au hasard
        bar_x[nbar] = bx;
        bar_y[nbar] = -PLUIE_H;
        bar_t[nbar] = AVERT + rnd(0, 100) * (PLUIE_ETALE / 100);                         // moment du depart
        nbar++;
        rect(bx, 0, PLUIE_L, 10, C_B);                // marqueur d'avertissement en haut
      }
      pluie_etat = 2;
    } else if (pluie_etat == 2) {
      int tr = now - pluie_t0, w = 0;
      // 1) on efface ce que les traits laissent derriere eux
      for (i = 0; i < nbar; i++) {
        int yn;
        if (tr < bar_t[i]) {                          // pas encore parti : son marqueur reste
          bar_x[w] = bar_x[i]; bar_y[w] = bar_y[i]; bar_t[w] = bar_t[i]; w++;
          continue;
        }
        yn = -PLUIE_H + PLUIE_V * (tr - bar_t[i]) / 1000;
        if (yn > bar_y[i]) {
          int e0 = maxi(0, bar_y[i]), e1 = mini(LH, mini(bar_y[i] + PLUIE_H, yn));
          if (e1 > e0) {
            rect(bar_x[i], e0, PLUIE_L, e1 - e0, C_N);
            bordures(bar_x[i], bar_x[i] + PLUIE_L);     // on repare les bordures traversees
          }
        }
        bar_y[i] = yn;
        if (yn < LH) { bar_x[w] = bar_x[i]; bar_y[w] = bar_y[i]; bar_t[w] = bar_t[i]; w++; }
      }
      nbar = w;
      // 2) on les redessine plus bas, et on teste la collision
      for (i = 0; i < nbar; i++) {
        if (tr >= bar_t[i]) {
          rect(bar_x[i], bar_y[i], PLUIE_L, PLUIE_H, C_B);
          if (inter(nx, ny, T, T, bar_x[i], bar_y[i], PLUIE_L, PLUIE_H)) return R_TRAIT;
        }
      }
      if (nbar == 0) {                                // tous les traits sont passes
        bordures(0, LW);
        pluie_etat = 0;
        na = now + cadence(DUREE * 1000 - (now - t0));
        ancien = -1;
      }
    }

    // deplacement du joueur (on efface l'ancienne place)
    if (ox != nx || oy != ny) restaurer(ox, oy, T, T);

    // collisions
    for (i = 0; i < natt; i++) {
      int *a = atts[i];
      if (a[ET] == 1) {
        int touche;
        if (a[TY] == 1) {
          int qx = maxi(nx, mini(a[P13], nx + T)), qy = maxi(ny, mini(a[P14], ny + T));
          touche = (qx - a[P13]) * (qx - a[P13]) + (qy - a[P14]) * (qy - a[P14]) <= a[P15] * a[P15];
        } else {
          touche = inter(nx, ny, T, T, a[X], a[Y], a[W], a[H]);
          if (touche && !a[CT]) {
            // laser : parry possible si on est pile au croisement
            if (absi(nx - a[P13]) <= PARRY_TOL && absi(ny - a[P14]) <= PARRY_TOL) {
              if (parry) a[P16] = 1;
              if (a[P16]) touche = 0;
            }
          }
        }
        if (touche) { rect(nx, ny, T, T, C_R); return R_ATT; }
      }
    }

    // compte a rebours au-dessus de la tete (zone interdite ou immobilite)
    {
      int cm = -1, lx, ly;
      if (rz_on) cm = rz;
      if (ri <= IDLE_ALERTE * 1000 && (cm < 0 || ri < cm)) cm = ri;
      n_aff = 0;
      if (cm >= 0) n_aff = mini(5, cm / 1000 + 1);
      lx = maxi(0, mini(LW - 10, nx - 1));
      ly = ny - 20;
      if (ly < 0) ly = ny + T + 2;
      if (lab_on && (n_aff == 0 || lab_x != lx || lab_y != ly || lab_n != n_aff)) {
        restaurer(lab_x, lab_y, 10, 18);
        lab_on = 0;
      }
      if (n_aff > 0) {
        char buf[4];
        itos(n_aff, buf);
        txt(buf, lx, ly, C_B, C_N);
        lab_on = 1; lab_x = lx; lab_y = ly; lab_n = n_aff;
      }
    }

    rect(nx, ny, T, T, parry ? C_P : C_R);

    // chrono (pas redessine pendant le mur ni la pluie : evite un carre noir sur les traits)
    reste = DUREE - (now - t0) / 1000;
    if (reste != ancien && mur_etat != 2 && pluie_etat != 2) {
      char buf[16];
      itos(reste, buf);
      scat(buf, "  ");
      txt(buf, IN + 2, IN + 2, C_B, C_N);
      ancien = reste;
    }

    eadk_timing_msleep(10);
  }
}

// ----------------------------------------------------------------------------
//  Menus
// ----------------------------------------------------------------------------
static const char *const NOMS[NATT] = {
  "Lasers", "Carre", "Cercles", "Refuge", "Diagonales", "Mur", "Damier", "Pluie"};
static int g_sel, g_ph;

// choix de l'attaque (haut/bas) et de la phase (gauche/droite), OK pour lancer,
// touche 3 pour revenir a l'ecran de depart. Renvoie 1 si on lance, 0 sinon.
static int menu_test(void) {
  int sel = 0, ph = 0, i;
  rect(0, 0, LW, LH, C_N);
  txt("Haut/Bas : attaque  <> : phase", 10, 178, C_B, C_N);
  txt("OK : lancer   3 : retour", 10, 198, C_B, C_N);
  eadk_timing_msleep(400);
  for (;;) {
    char buf[40];
    for (i = 0; i < NATT; i++) {
      int k;
      buf[0] = (i == sel) ? '>' : ' ';
      buf[1] = ' ';
      buf[2] = 0;
      scat(buf, NOMS[i]);
      for (k = slen(buf); k < 14; k++) buf[k] = ' ';
      buf[14] = 0;
      txt(buf, 20, 19 + 17 * i, C_B, C_N);
    }
    buf[0] = 0;
    scat(buf, "MODE TEST   Phase : ");
    {
      char nb[4];
      itos(ph + 1, nb);
      scat(buf, nb);
    }
    scat(buf, " ");
    txt(buf, 20, 0, C_B, C_N);
    for (;;) {
      scan();
      if (key(K_UP)) { sel = (sel + NATT - 1) % NATT; break; }
      if (key(K_DOWN)) { sel = (sel + 1) % NATT; break; }
      if (key(K_LEFT)) { ph = (ph + NPAL - 1) % NPAL; break; }
      if (key(K_RIGHT)) { ph = (ph + 1) % NPAL; break; }
      if (key(K_OK) || key(K_EXE)) { eadk_timing_msleep(300); g_sel = sel; g_ph = ph; return 1; }
      if (key(K_THREE)) { eadk_timing_msleep(300); return 0; }
      eadk_timing_msleep(30);
    }
    eadk_timing_msleep(180);
  }
}

static void attendre(void) {
  eadk_timing_msleep(600);
  for (;;) {
    scan();
    if (key(K_OK) || key(K_EXE)) return;
    eadk_timing_msleep(50);
  }
}

// 1 = jouer, 2 = mode test, 0 = quitter l'application (touche Back ou Home)
static int ecran_depart(void) {
  char buf[40], nb[8];
  rect(0, 0, LW, LH, C_N);
  buf[0] = 0;
  scat(buf, "SURVIS ");
  itos(DUREE, nb);
  scat(buf, nb);
  scat(buf, " SECONDES");
  txt(buf, 70, 60, C_B, C_N);
  txt("OK : jouer", 100, 100, C_B, C_N);
  txt("3 : mode test", 90, 125, C_B, C_N);
  txt("En jeu : OK = parry", 70, 160, C_B, C_N);
  txt("Back : quitter", 90, 190, C_B, C_N);
  eadk_timing_msleep(600);
  for (;;) {
    scan();
    if (key(K_OK) || key(K_EXE)) return 1;
    if (key(K_THREE)) return 2;
    if (key(K_BACK) || key(K_HOME)) return 0;
    eadk_timing_msleep(50);
  }
}

static void fin(int res) {
  rect(0, 0, LW, LH, C_N);
  if (res == R_V) {
    txt("VICTOIRE !", 110, 90, C_B, C_N);
  } else {
    txt("GAME OVER", 115, 80, C_R, C_N);
    txt(MSG[res], (LW - 10 * slen(MSG[res])) / 2, 105, C_B, C_N);
  }
  txt("OK pour continuer", 80, 140, C_B, C_N);
  attendre();
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  t_base = (uint32_t)eadk_timing_millis();
  rng_seed((uint32_t)eadk_timing_millis() * 2654435761u + 12345u);
  init_tables();
  for (;;) {
    int choix = ecran_depart();
    if (choix == 0) return 0;
    if (choix == 2) {
      while (menu_test()) {
        int res = jeu(g_sel, g_ph);
        if (res != R_MENU) fin(res);
      }
    } else {
      int res = jeu(-1, 0);
      if (res != R_MENU) fin(res);
    }
  }
}
