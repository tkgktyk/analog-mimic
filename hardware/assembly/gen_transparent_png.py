import argparse
from PIL import Image, ImageDraw

def generate_image(input_path, output_path, tolerance=10):
    try:
        img = Image.open(input_path).convert("RGBA")
    except FileNotFoundError:
        print(f"エラー: 画像ファイル '{input_path}' が見つかりません。")
        return

    datas = img.getdata()
    new_data = []

    # 1. クロマキー透過処理
    for item in datas:
        r, g, b, a = item
        if r < tolerance and g > (255 - tolerance) and b < tolerance:
            new_data.append((0, 0, 0, 0)) # 一旦完全に透過させる
        else:
            new_data.append(item)

    img.putdata(new_data)

    # 2. 自動トリミング（緑だった部分をカット）
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)

    # 3. 画像サイズを半分(50%)に縮小
    new_width = img.width // 2
    new_height = img.height // 2
    img = img.resize((new_width, new_height), Image.Resampling.LANCZOS)

    # 4. 透過部分（背景）を黒にする
    black_bg = Image.new("RGBA", img.size, (0, 0, 0, 255))
    img = Image.alpha_composite(black_bg, img)

    # 5. 最外周1pxを黒で塗る（枠線）
    draw = ImageDraw.Draw(img)
    draw.rectangle([(0, 0), (new_width - 1, new_height - 1)], outline=(0, 0, 0, 255), width=1)

    # 指定された出力パスで保存
    img.save(output_path, "PNG")
    print(f"保存完了: {output_path} (サイズ: {new_width}x{new_height})")

if __name__ == "__main__":
    # コマンドライン引数の設定
    parser = argparse.ArgumentParser(description="背景透過、自動トリミング、50%縮小、黒背景化を行うスクリプト")
    
    # 位置引数（第一引数、第二引数）として定義
    parser.add_argument("input_file", help="処理する元の画像ファイルのパス（例: mimic01.png）")
    parser.add_argument("output_file", help="出力する画像ファイルのパス（例: mimic01_pcb.png）")
    
    # オプション引数
    parser.add_argument(
        "--tolerance", 
        type=int, 
        default=10, 
        help="緑色の判定閾値（デフォルト: 10）"
    )

    args = parser.parse_args()
    
    # 引数を関数に渡して実行
    generate_image(args.input_file, args.output_file, args.tolerance)
