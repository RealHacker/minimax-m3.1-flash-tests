from PIL import Image, ImageDraw

# 1. Load the image (replace 'ski_picture.jpg' with your actual file name)
image_path = 'findwaldo.jpg'
img = Image.open(image_path)

# 2. Create a drawing context
draw = ImageDraw.Draw(img)

# 3. Define the bounding box coordinates for Waldo
# These are based on an assumed image size of roughly 1000x800 pixels.
# If your image is a different resolution, you will need to scale these.
x1, y1 = 700, 630  # Top-left corner
x2, y2 = 760, 710  # Bottom-right corner

# 4. Draw the red rectangle
# 'outline' sets the color, 'width' sets the thickness of the line
draw.rectangle([x1, y1, x2, y2], outline="red", width=5)

# 5. Save and show the result
output_path = 'waldo_found.jpg'
img.save(output_path)
img.show()

print(f"Image saved to {output_path}")