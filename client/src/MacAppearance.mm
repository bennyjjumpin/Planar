#include "MacAppearance.h"

#import <AppKit/AppKit.h>

void UseLightAppearance() {
    NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
}
