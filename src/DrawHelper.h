#pragma once
#include <LovyanGFX.hpp>

// Simple 32x32 Refresh Icon (XBM style) or similar
// For now, using Primitives for Bins to ensure they look "Vector-like"

class DrawHelper {
public:
    static void drawBin(LGFX_Sprite* sprite, int x, int y, int height, uint16_t color, String type) {
        // Draw a generic Wheelie Bin shape
        // Height approx 60px? Let's scale based on height arg.
        // Base width = height * 0.6
        
        int w = height * 0.6;
        int lidH = height * 0.15;
        int bodyH = height * 0.75;
        int wheelR = height * 0.08;
        
        // Body
        sprite->fillRect(x + w*0.1, y + lidH, w*0.8, bodyH, color);
        
        // Lid (Trapezoid-ish)
        sprite->fillTriangle(x, y + lidH, x + w, y + lidH, x + w*0.1, y, color);
        sprite->fillTriangle(x + w*0.1, y, x + w*0.9, y, x + w, y + lidH, color);
        
        // Wheel
        sprite->fillCircle(x + w*0.1, y + height - wheelR, wheelR, color);
        sprite->fillCircle(x + w*0.9, y + height - wheelR, wheelR, color);
        
        // Label (White text on the bin)
        sprite->setTextColor(TFT_WHITE);
        sprite->setTextSize(1);
        sprite->setTextDatum(middle_center);
        
        String letter = "";
        if (type == "RUBBISH") letter = "GEN";
        else if (type == "RECYCLING") letter = "REC";
        else if (type == "FOOD") letter = "FD"; // Food usually small bin
        else if (type == "GARDEN") letter = "GRD";
        
        sprite->drawString(letter, x + w/2, y + lidH + bodyH/2);
    }
    
    // Draw Rotate Text for "Last Updated"
    static void drawVerticalStatus(LGFX_Sprite* sprite, String timeStr) {
        sprite->setTextDatum(middle_center);
        // Draw rotated text on the left edge
        // LGFX doesn't support arbitrary rotation of text easily without rotating sprite?
        // Actually it does! sprite.setRotation?
        // But we want to draw onto the main sprite.
        // We can create a small temporary sprite, draw text, pushRotate.
        
        // But E-Ink is 128px high. 
        // We want text running bottom-up or top-down on the left edge.
        // "Updated: 18:09"
        
        // Simple hack: Draw character by character? Or just rotate the whole canvas 90 deg?
        // No, mixed content.
        
        // Use pushRotate
        /*
        LGFX_Sprite status(sprite);
        status.createSprite(128, 20); // Width is actually height of the main
        status.fillSprite(TFT_WHITE);
        status.setTextColor(TFT_BLACK);
        status.drawString(timeStr, 64, 10);
        status.pushRotateZoom(10, 64, 90, 1, 1); // x, y, angle, zoom_x, zoom_y
        status.deleteSprite();
        */
        // Simpler: Just write horizontal text at the bottom or top left if rotation is hard.
        // User asked "left of screen vertically".
        // Let's try Standard Font rendering char by char if pushRotate is heavy.
        
        int x = 5;
        int y = 10;
        for (int i=0; i<timeStr.length(); i++) {
            sprite->setCursor(x, y);
            sprite->print(timeStr[i]);
            y += 10;
        }
    }
};
