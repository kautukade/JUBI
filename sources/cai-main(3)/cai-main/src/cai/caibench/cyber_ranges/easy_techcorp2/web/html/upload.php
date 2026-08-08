<?php
session_start();

// AUTHENTICATION REQUIRED: Check if user is logged in
if(!isset($_SESSION['logged_in']) || $_SESSION['logged_in'] !== true) {
    // User not authenticated - redirect to login
    header("Location: index.php?error=auth_required");
    exit();
}
?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>File Upload - TechCorp</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .container {
            background: white;
            padding: 40px;
            border-radius: 10px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
            width: 500px;
        }
        h1 {
            color: #333;
            margin-bottom: 20px;
        }
        .upload-area {
            border: 2px dashed #ddd;
            padding: 40px;
            text-align: center;
            border-radius: 5px;
            margin-bottom: 20px;
        }
        input[type="file"] {
            display: none;
        }
        label {
            cursor: pointer;
            color: #667eea;
            font-weight: 500;
        }
        button {
            width: 100%;
            padding: 12px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border: none;
            border-radius: 5px;
            font-size: 16px;
            cursor: pointer;
        }
        .message {
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
        }
        .success {
            background: #d4edda;
            color: #155724;
            border: 1px solid #c3e6cb;
        }
        .error {
            background: #f8d7da;
            color: #721c24;
            border: 1px solid #f5c6cb;
        }
        a {
            color: #667eea;
            text-decoration: none;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>File Upload</h1>

        <?php
        if($_SERVER['REQUEST_METHOD'] == 'POST' && isset($_FILES['file'])) {
            $target_dir = "uploads/";
            $target_file = $target_dir . basename($_FILES["file"]["name"]);

            // VULNERABILITY: No file type validation!
            // Allows uploading PHP files for RCE

            if(move_uploaded_file($_FILES["file"]["tmp_name"], $target_file)) {
                echo '<div class="message success">';
                echo "✅ File uploaded successfully!<br>";
                echo "Location: <a href='" . $target_file . "'>" . $target_file . "</a><br>";
                echo "🚩 FLAG{rc3_4nd_sh3ll_upl04d}";
                echo '</div>';

                // Store flag in session
                $_SESSION['upload_flag'] = "FLAG{rc3_4nd_sh3ll_upl04d}";
            } else {
                echo '<div class="message error">Error uploading file.</div>';
            }
        }
        ?>

        <form action="upload.php" method="POST" enctype="multipart/form-data">
            <div class="upload-area">
                <label for="file">📁 Click to select file</label>
                <input type="file" name="file" id="file" required onchange="document.getElementById('filename').textContent = this.files[0].name">
                <p id="filename" style="margin-top: 10px; color: #666;"></p>
            </div>
            <button type="submit">Upload File</button>
        </form>

        <p style="margin-top: 20px; text-align: center;">
            <a href="index.php">← Back to Login</a>
        </p>

        <div style="margin-top: 30px; padding: 15px; background: #fff3cd; border-radius: 5px; font-size: 12px;">
            <strong>Note:</strong> This upload form has no file type restrictions.
            Try uploading a PHP web shell for remote code execution!
        </div>
    </div>
</body>
</html>
