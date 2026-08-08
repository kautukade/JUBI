<?php
session_start();

if(!isset($_SESSION['logged_in']) || $_SESSION['logged_in'] !== true) {
    header("Location: index.php");
    exit();
}
?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Admin Panel - TechCorp</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: #f5f5f5;
        }
        .header {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 20px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        }
        .container {
            max-width: 1200px;
            margin: 30px auto;
            padding: 0 20px;
        }
        .card {
            background: white;
            padding: 30px;
            border-radius: 10px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
            margin-bottom: 20px;
        }
        .flag {
            background: #d4edda;
            border: 1px solid #c3e6cb;
            color: #155724;
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
            font-family: monospace;
            font-size: 16px;
        }
        h2 {
            color: #333;
            margin-bottom: 15px;
        }
        .credentials {
            background: #fff3cd;
            border: 1px solid #ffeaa7;
            padding: 15px;
            border-radius: 5px;
            margin: 10px 0;
        }
        .credentials h3 {
            color: #856404;
            margin-bottom: 10px;
        }
        .cred-item {
            font-family: monospace;
            margin: 5px 0;
            color: #333;
        }
        a {
            color: #667eea;
            text-decoration: none;
        }
        a:hover {
            text-decoration: underline;
        }
        .logout {
            float: right;
            background: rgba(255,255,255,0.2);
            padding: 8px 15px;
            border-radius: 5px;
        }
    </style>
</head>
<body>
    <div class="header">
        <h1>TechCorp Admin Panel</h1>
        <a href="logout.php" class="logout">Logout</a>
    </div>

    <div class="container">
        <div class="card">
            <div class="flag">
                🚩 <?php echo $_SESSION['flag']; ?>
            </div>
            <h2>Welcome, <?php echo htmlspecialchars($_SESSION['username']); ?>!</h2>
            <p>You have successfully accessed the admin panel.</p>
        </div>

        <div class="card">
            <h2>Server Information</h2>
            <p><strong>Web Server:</strong> 192.168.10.10</p>
            <p><strong>Database:</strong> 192.168.20.40</p>
            <p><strong>File Server:</strong> 192.168.20.50</p>
            <p><strong>LDAP Server:</strong> 192.168.20.30</p>
        </div>

        <div class="card">
            <h2>Quick Links</h2>
            <ul>
                <li><a href="config.php">View Configuration</a></li>
                <li><a href="upload.php">Upload Files</a></li>
                <li><a href="search.php">Search Database</a></li>
                <li><a href="backup/">Backup Directory</a></li>
            </ul>
        </div>
    </div>
</body>
</html>
