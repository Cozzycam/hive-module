// HiveRelay case — Adafruit HUZZAH32 Feather
// Board: 52mm x 28mm x 18mm (L x W x H)
// USB slot: 10mm x 7mm on short side

// --- Parameters ---
board_l = 52;    // sides A+C (long)
board_w = 28;    // sides B+D (short)
board_h = 18;    // thickness

wall    = 2;     // wall thickness
tol     = 0.4;   // clearance per side
lip     = 1.5;   // lid press-fit lip height
lip_t   = 1.2;   // lip thickness

usb_w   = 10;    // USB slot width
usb_h   = 7;     // USB slot height

// --- Derived ---
inner_l = board_l + tol * 2;
inner_w = board_w + tol * 2;
inner_h = board_h + tol;

outer_l = inner_l + wall * 2;
outer_w = inner_w + wall * 2;
outer_h = inner_h + wall;        // base wall only (no top wall — lid covers)

lid_h   = wall + lip;

// --- Base ---
module base() {
    difference() {
        // Outer shell
        cube([outer_l, outer_w, outer_h]);

        // Inner cavity
        translate([wall, wall, wall])
            cube([inner_l, inner_w, inner_h + 1]);  // +1 open top

        // USB slot on short side (x=0), near top but below lip
        translate([-0.1, outer_w/2 - usb_w/2, outer_h - usb_h - lip - wall])
            cube([wall + 0.2, usb_w, usb_h]);
    }

    // Lip ledge inside walls for lid to rest on
    // (inset ridge around top inner perimeter)
    translate([wall, wall, outer_h - lip])
    difference() {
        cube([inner_l, inner_w, lip]);
        translate([lip_t, lip_t, -0.1])
            cube([inner_l - lip_t*2, inner_w - lip_t*2, lip + 0.2]);
    }
}

// --- Lid ---
module lid() {
    // Flat top
    cube([outer_l, outer_w, wall]);

    // Press-fit tongue (fits inside the lip ledge)
    translate([wall + lip_t + tol/2, wall + lip_t + tol/2, wall])
        cube([inner_l - lip_t*2 - tol, inner_w - lip_t*2 - tol, lip - 0.2]);
}

// --- Layout for printing ---
base();

translate([0, outer_w + 10, 0])
    lid();
